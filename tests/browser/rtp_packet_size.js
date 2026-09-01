/* Reads back the size of the RTP video packets actually received.
 * WebRTC reports bytesReceived and packetsReceived, so the average
 * payload size per packet is directly observable -- which is how we
 * check the payloader MTU really took effect on the wire. */
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto(`http://${process.argv[2] || '127.0.0.1'}:5080/`, { waitUntil: 'networkidle' });
  await p.waitForTimeout(8000);
  const r = await p.evaluate(async () => {
    const stats = await pc.getStats();
    let v = null;
    stats.forEach(s => {
      if (s.type === 'inbound-rtp' && s.kind === 'video') v = s;
    });
    return v && { bytes: v.bytesReceived, packets: v.packetsReceived, frames: v.framesDecoded };
  });
  if (!r || !r.packets) { console.log('no video received'); process.exit(1); }
  console.log(`paquets: ${r.packets}, octets: ${r.bytes}, frames decodees: ${r.frames}`);
  console.log(`taille moyenne par paquet RTP: ${(r.bytes / r.packets).toFixed(0)} octets`);
  await b.close();
})();
