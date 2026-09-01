/* Prints the ICE candidates each side advertises. With no STUN/TURN
 * configured, the only way a remote peer can connect is if one of the
 * server's HOST candidates is an address that peer can actually reach. */
const { chromium } = require('playwright');
const HOST = process.argv[2] || '127.0.0.1';
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto(`http://${HOST}:5080/`, { waitUntil: 'networkidle' });
  await p.waitForTimeout(4000);
  const out = await p.evaluate(() => ({
    state: pc && pc.iceConnectionState,
    conn: pc && pc.connectionState,
    local: (pc && pc.localDescription ? pc.localDescription.sdp : '')
      .split('\n').filter(l => l.includes('a=candidate')).map(l => l.trim()),
    remote: (pc && pc.remoteDescription ? pc.remoteDescription.sdp : '')
      .split('\n').filter(l => l.includes('a=candidate')).map(l => l.trim())
  }));
  console.log('ice:', out.state, '/ connection:', out.conn);
  console.log('\n--- candidats annonces par LE NAVIGATEUR ---');
  out.local.forEach(c => console.log('  ' + c));
  console.log('\n--- candidats annonces par LE SERVEUR (capture2cloud) ---');
  out.remote.forEach(c => console.log('  ' + c));
  await b.close();
})();
