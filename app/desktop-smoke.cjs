const { _electron: electron } = require('playwright');
const path = require('path');
const fs = require('fs');
(async () => {
  const port = process.env.SMOKE_PORT || '8765';
  const env = {...process.env, LOCALTALKER_PORT: port};
  delete env.ELECTRON_RUN_AS_NODE;
  const wav = process.env.SMOKE_WAV || path.resolve(__dirname, '../.localtalker/validation/mic-input.wav');
  const args = process.env.SMOKE_EXE ? [] : [path.resolve(__dirname)];
  if (process.env.LOCALTALKER_SMOKE === '1') args.push('--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', `--use-file-for-fake-audio-capture=${wav}`);
  const app = await electron.launch({args, executablePath: process.env.SMOKE_EXE || undefined, env, timeout:60000});
  let characterId;
  const errors = [];
  const events = [];
  try {
    const page = await app.firstWindow();
    page.on('pageerror', error => errors.push(error.message));
    page.on('websocket', ws => ws.on('framereceived', frame => {events.push(JSON.parse(String(frame.payload)));}));
    await page.waitForSelector('.character-heading', {timeout:60000});
    console.log('Native desktop bridge:', await page.evaluate(() => window.localtalker?.desktop));
    console.log('Mic inputs:', await page.evaluate(async () => (await navigator.mediaDevices.enumerateDevices()).filter(d=>d.kind==='audioinput').length));
    if (process.env.LOCALTALKER_SMOKE === '1') {
      const base = `http://127.0.0.1:${port}`;
      const char = await (await fetch(`${base}/v1/characters`, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name:'Desktop validation', personality:'A friendly innkeeper named Mira.', instructions:'Answer in one short sentence.'})})).json();
      characterId = char.id;
      await page.reload();
      await page.locator('.char').filter({hasText:'Desktop validation'}).click();
      await page.getByText('Ready', {exact:true}).waitFor();
      await page.getByLabel('Message', {exact:true}).fill('Hello. Can I have a warm meal?');
      await page.getByRole('button', {name:'Send', exact:true}).click();
      await page.waitForFunction(() => document.querySelectorAll('.line.assistant .chips').length > 0, undefined, {timeout:60000});
      await page.getByRole('button', {name:'Interrupt', exact:true}).click();
      await page.getByText('Ready', {exact:true}).waitFor();
      const before = events.length;
      await page.getByRole('button', {name:'Microphone', exact:true}).hover();
      await page.mouse.down();
      await page.waitForTimeout(Number(process.env.SMOKE_CAPTURE_MS || 6500));
      await page.mouse.up();
      await page.waitForFunction(() => document.querySelectorAll('.line.assistant .chips').length >= 2, undefined, {timeout:60000});
      const speechEvents = events.slice(before);
      if (!speechEvents.some(e=>e.type==='transcript' && new RegExp(process.env.SMOKE_EXPECT || 'meal','i').test(e.transcript))) throw new Error('Microphone did not transcribe the known recording');
      if (!speechEvents.some(e=>e.type==='audio' && e.pcm16_b64)) throw new Error('No speech output');
      if (speechEvents.some(e=>e.type==='reply' && e.extra.provider_id==='mock')) throw new Error('Unexpected mock inference');
      console.log('Real desktop microphone turn:', speechEvents.filter(e=>['transcript','reply'].includes(e.type)).map(e=>({type:e.type,text:e.transcript || e.text,timing:e.timing?.totals_ms})));
      await page.getByRole('button', {name:'Interrupt', exact:true}).click();
      await page.getByRole('button', {name:'Inspect', exact:true}).click();
      await page.getByRole('button', {name:'timing', exact:true}).click();
    }
    fs.mkdirSync(path.resolve(__dirname,'../.localtalker/validation'), {recursive:true});
    await page.screenshot({path:path.resolve(__dirname,'../.localtalker/validation/desktop.png'), animations:'disabled'});
    if (errors.length) throw new Error(errors.join('\n'));
    console.log('Desktop smoke passed');
  } finally {
    if (characterId) await fetch(`http://127.0.0.1:${port}/v1/characters/${characterId}`, {method:'DELETE'});
    await app.close();
  }
})().catch(e=>{console.error(e);process.exit(1)});
