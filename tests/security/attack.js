/* Fires real attack traffic at a running instance and reports what came
 * back. Raw sockets rather than fetch(), because several of these are
 * deliberately malformed in ways an HTTP client would refuse to send.
 *
 * Deliberately never completes a /wake with a valid token: that
 * power-cycles the console's mains supply. Its refusal path is what
 * matters, and that is testable without one.
 */
const net = require('net');
const HOST = process.env.C2C_HOST || '127.0.0.1';
const PORT = Number(process.env.C2C_PORT || 5080);

let pass = 0, fail = 0;
function check(name, ok, detail) {
  if (ok) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}${detail ? '  <- ' + detail : ''}`); }
}
function group(n) { console.log(`\n${n}`); }

function raw(payload, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const s = net.connect(PORT, HOST);
    let buf = '';
    const done = (r) => { try { s.destroy(); } catch (e) {} resolve(r); };
    s.setTimeout(timeoutMs, () => done(buf || '<timeout>'));
    s.on('connect', () => s.write(payload));
    s.on('data', (d) => { buf += d.toString('latin1'); });
    s.on('close', () => done(buf));
    /* A refusal that closes without draining the request body makes the
     * kernel reset the connection; the response is already buffered, so
     * keep it rather than reporting a transport error. */
    s.on('error', (e) => done(buf || '<error ' + e.code + '>'));
  });
}
const status = (r) => (r.match(/^HTTP\/1\.1 (\d{3})/) || [])[1] || r.slice(0, 20);
const body = (r) => (r.split('\r\n\r\n')[1] || '');
const req = (method, path, headers = '', payload = '') =>
  raw(`${method} ${path} HTTP/1.1\r\nHost: ${HOST}\r\n${headers}` +
      (payload ? `Content-Length: ${Buffer.byteLength(payload)}\r\n` : '') +
      `Connection: close\r\n\r\n${payload}`);

(async () => {
  console.log(`cible: http://${HOST}:${PORT}`);

  group('path traversal / divulgation de fichiers');
  for (const p of [
    '/../../../../etc/passwd', '/..%2f..%2f..%2fetc/passwd',
    '/%2e%2e/%2e%2e/etc/passwd', '/./../scripts/.env', '/scripts/.env',
    '/../scripts/.env', '/app.js/../scripts/.env', '/capture2cloud.c',
    '/.env', '/page.html%00.js', '//etc/passwd',
  ]) {
    const r = await req('GET', p);
    const leaked = /root:|PLAYER_PASSWORD|HA_TOKEN|#include/.test(r);
    check(`${p} -> ${status(r)}, rien ne fuit`, status(r) === '404' && !leaked,
      leaked ? 'CONTENU DIVULGUE' : 'statut ' + status(r));
  }

  group('controle d\'acces (sans jeton)');
  check('POST /wake refuse', status(await req('POST', '/wake')) === '403');
  check('POST /quality refuse', status(await req('POST', '/quality', '', '4000')) === '403');
  /* Comme /wake, jamais complete avec un jeton valable : cela couperait
     le flux de tout le monde et arreterait la suite au milieu. */
  check('POST /restart refuse', status(await req('POST', '/restart')) === '403');
  check('POST /reset-dongle refuse', status(await req('POST', '/reset-dongle')) === '403');
  for (const bogus of ['deadbeef', '0'.repeat(64), 'a'.repeat(200), '../../etc']) {
    const r = await req('POST', '/wake', `X-Player-Token: ${bogus}\r\n`);
    check(`jeton bidon (${bogus.slice(0, 10)}...) refuse`, status(r) === '403', status(r));
    const rr = await req('POST', '/restart', `X-Player-Token: ${bogus}\r\n`);
    check(`jeton bidon sur /restart refuse`, status(rr) === '403', status(rr));
  }

  group('injection d\'en-tete');
  const inj = await raw('POST /wake HTTP/1.1\r\nHost: x\r\nX-Player-Token: abc\r\n' +
                        'X-Injected: yes\r\nConnection: close\r\n\r\n');
  check('un en-tete ajoute ne forge pas d\'autorisation', status(inj) === '403', status(inj));
  const crlf = await req('GET', '/clients', 'X-Player-Token: a%0d%0aX-Evil: 1\r\n');
  check('pas d\'injection reflechie', !/X-Evil/i.test(crlf));

  group('Content-Length malforme');
  for (const cl of ['-1', '0', '99999999999999999999', '9223372036854775807', 'abc']) {
    const r = await raw(`POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: ${cl}\r\nConnection: close\r\n\r\nx`);
    check(`Content-Length: ${cl} gere sans casse`, status(r).length === 3 || status(r) === '<timeout>', status(r));
  }

  group('requetes surdimensionnees / malformees');
  check('chemin tres long', status(await req('GET', '/' + 'A'.repeat(8000))).length === 3);
  check('ligne d\'en-tete tres longue', status(await req('GET', '/', 'X-Big: ' + 'A'.repeat(9000) + '\r\n')).length === 3);
  check('200 en-tetes inutiles', status(await req('GET', '/', 'X-H: v\r\n'.repeat(200))).length === 3);
  check('methode inconnue', status(await req('DELETE', '/')) === '404');
  check('GET sur un endpoint POST', status(await req('GET', '/wake')) === '404');
  check('POST sur un endpoint GET', status(await req('POST', '/app.js', '', 'x')) === '404');
  const nul = await raw('GET /app.js\x00/../.env HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n');
  check('octet NUL dans le chemin ne divulgue rien', !/PLAYER_PASSWORD|HA_TOKEN/.test(nul));

  group('login');
  /* This suite deliberately triggers the failed-login lockout further
   * down, and it lasts 30 s -- longer than a run. Two runs back to back
   * would otherwise see 429 here and report a false failure, which is
   * exactly what happened. Wait it out rather than measure through it. */
  for (let i = 0; i < 40; i++) {
    if (status(await req('POST', '/login', '', 'x')) !== '429') break;
    await new Promise((r) => setTimeout(r, 1000));
  }
  const wrong = await req('POST', '/login', '', 'definitely-not-the-password');
  check('mauvais mot de passe -> 401', status(wrong) === '401', status(wrong));
  check('le 401 ne donne aucun indice', !/password|expected|changeme/i.test(body(wrong)));
  const t0 = Date.now();
  await req('POST', '/login', '', 'wrong-again');
  const delay = Date.now() - t0;
  check(`echec de login temporise (${delay} ms)`, delay >= 400, `${delay} ms`);

  group('divulgation d\'information');
  const root = await req('GET', '/');
  check('pas de banniere Server:', !/^Server:/im.test(root));
  check('la page ne contient pas le mot de passe', !/changeme/i.test(root));
  const cl2 = await req('GET', '/clients');
  check('/clients lisible sans jeton (par conception)', status(cl2) === '200', status(cl2));
  check('/clients ne revele qu\'un compteur', /^\d+\/\d+$/.test(body(cl2).trim()), body(cl2).trim());

  group('la temporisation de login resiste-t-elle au parallelisme ?');
  {
    /* The 0.5 s penalty blocks only the guessing connection's own
     * thread. Fired in parallel, the wall-clock cost of N guesses is the
     * cost of one -- so the delay slows a single-threaded attacker and
     * almost nobody else. */
    const N = 30;
    const t = Date.now();
    const rs = await Promise.all(Array.from({ length: N }, (_, i) =>
      req('POST', '/login', '', 'guess-' + i)));
    const total = Date.now() - t;
    const refused = rs.filter((r) => status(r) === '429').length;
    const tried = rs.filter((r) => status(r) === '401').length;
    console.log(`  ${N} tentatives en parallele en ${total} ms : ${tried} evaluees, ${refused} bloquees`);
    check('le parallelisme ne permet pas N essais', tried < N, `${tried} evaluees sur ${N}`);
    check('le surplus est repousse en 429', refused > 0, `${refused} en 429`);
    /* Even the right password must be refused while locked, or guessing
     * through the lockout would still work. */
    const good = await req('POST', '/login', '', 'changeme');
    check('meme le bon mot de passe est refuse pendant le blocage', status(good) === '429', status(good));
  }

  group('origine croisee / CSRF');
  {
    const r = await req('POST', '/wake', 'Origin: http://evil.example\r\n');
    check('pas d\'en-tete CORS permissif dans la reponse',
      !/Access-Control-Allow-Origin/i.test(r), 'CORS present');
    check('/wake d\'une autre origine reste refuse sans jeton', status(r) === '403', status(r));
    const h = await req('GET', '/', 'Host: evil.example\r\n');
    check('l\'en-tete Host n\'est pas valide (rebinding DNS possible)',
      status(h) === '200', 'note: informatif');
  }

  group('epuisement de connexions');
  {
    const N = 60;
    const t = Date.now();
    const rs = await Promise.all(Array.from({ length: N }, () => req('GET', '/clients')));
    const okCount = rs.filter((r) => status(r) === '200').length;
    console.log(`  ${N} connexions simultanees : ${okCount} servies en ${Date.now() - t} ms`);
    check('le serveur survit et repond encore', status(await req('GET', '/clients')) === '200');
  }

  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
