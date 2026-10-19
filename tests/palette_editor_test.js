'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const C = require('../tools/palette-editor/palette-core.js');
const root = path.resolve(__dirname,'..');
const assets = path.join(root,'assets/VitaGram_UI_Assets_v1');
const defaults = JSON.parse(fs.readFileSync(path.join(assets,'masks/manifest.json'))).palettes;
const files=['palettes/little-greene-light.json','palettes/little-greene-dark.json','palettes/black-red/palette.json','palettes/black-blue/palette.json'];
const I=require('../tools/palette-editor/i18n.js');
const translations=Object.fromEntries(fs.readdirSync(path.join(root,'tools/palette-editor/locales')).map(file=>[file.slice(0,-5),JSON.parse(fs.readFileSync(path.join(root,'tools/palette-editor/locales',file),'utf8'))]));
const english=(message,...args)=>I.text(translations,'en',message,...args);
assert.equal(I.choose(['fr-FR','en-US']),'en');
assert.equal(I.choose(['ru-RU','en']),'ru');
assert.equal(I.choose(['fr-FR']),'en');
for(const [tag,expected] of [['pt-BR','pt-BR'],['pt','pt-PT'],['zh-TW','zh-Hant'],['zh-HK','zh-Hant'],['zh-CN','zh-Hans'],['nb-NO','no'],['en-GB','en'],['fr-FR','fr']])assert.equal(I.choose([tag],['ru',...Object.keys(translations)]),expected);
assert.equal(I.text(translations,'ru','Светлая'),'Светлая');
assert.equal(english('Отсутствует роль: {0}','accent'),'Missing role: accent');
assert.equal(english('User color'),'User color');
for(const catalog of Object.values(translations)) {
  assert.deepEqual(Object.keys(catalog).sort(),Object.keys(translations.en).sort());
for(const [source,translated] of Object.entries(catalog)) {
    assert.equal(typeof translated,'string');assert(translated.length);
    assert.deepEqual([...source.matchAll(/\{\d+\}/g)].map(m=>m[0]).sort(),[...translated.matchAll(/\{\d+\}/g)].map(m=>m[0]).sort(),source);
  }
}
for(const name of ['editor.js','native-preview.js','palette-core.js']) {
  const source=fs.readFileSync(path.join(root,'tools/palette-editor',name),'utf8');
  for(const match of source.matchAll(/\b(?:t|status)\('([^']*[А-Яа-яЁё][^']*)'/g))assert(Object.hasOwn(translations.en,match[1]),name+': '+match[1]);
}
const template=fs.readFileSync(path.join(root,'tools/palette-editor/editor.html'),'utf8');
for(const match of template.matchAll(/data-i18n(?:-aria-label|-placeholder)?="([^"]+)"/g))assert(Object.hasOwn(translations.en,match[1]),match[1]);
assert.throws(()=>C.parse('{broken',defaults,english),/Invalid JSON/);
assert.throws(()=>C.filename('x'.repeat(97),english),/96 UTF-8 bytes/);
assert.throws(()=>C.parseCatalog('{"colors":[{"hex":"bad"}]}',english),/Color 1/);
assert.equal(C.parseCatalog('{"colors":[{"hex":"#123456"}]}',english).name,'My catalogue');
assert.equal(C.contrast('#000000','#FFFFFF'),21);
for (const file of files) {
  const palette = C.parse(fs.readFileSync(path.join(assets,file),'utf8'),defaults);
  assert.deepEqual(C.issues(palette),[],file);
  assert.deepEqual(C.parse(C.serialize(palette),defaults),palette);
  assert.deepEqual(C.adapt(palette),palette);
  const derived = C.derive(palette,defaults);
  assert.deepEqual(C.issues(derived),[],file);
  assert.equal(derived.accent,palette.accent);
  for(const [role,source] of [['icon','text'],['control_active','accent'],['control_inactive','muted'],['delivery','green']])
    assert.equal(palette[role],palette[source],file);
  const semantic={...palette,icon:'#ABCDEF',notification:'#F020B0',control_active:'#FFCC22',control_inactive:'#11CC88',delivery:'#FFE260'};
  const changed=C.derive({...semantic,accent:'#8855FF'},defaults);
  for(const role of ['icon','notification','control_active','control_inactive','delivery'])assert.equal(changed[role],semantic[role]);
  assert.deepEqual(C.parse(C.serialize(semantic),defaults),semantic);
  const borders={border:'#30F0B0',focus:'#FFD54A'};
  const independent=C.derive({...palette,accent:'#8855FF'},defaults,borders);
  assert.equal(independent.border,borders.border);
  assert.equal(independent.focus,borders.focus);
  assert.deepEqual(C.parse(C.serialize(independent),defaults),independent);
  assert.equal(C.derive(palette,defaults,{border:borders.border}).focus,palette.accent);
  for (const background of ['#000000','#FFFFFF','#777777','#0058A3']) {
    const altered = C.adapt({...palette,header_top:background,header_bottom:background,header_text:background});
    for (const role of ['header_text',...C.battery]) assert(C.contrast(altered[role],background)>=3);
  }
}
for(const name of ['light','dark'])assert.deepEqual(C.issues(C.adapt(defaults[name])),[],name);
for(const input of [...files.map(file=>JSON.parse(fs.readFileSync(path.join(assets,file)))),...Object.values(defaults),
  ...['bbr','brr2'].map(name=>JSON.parse(fs.readFileSync(path.join(__dirname,'fixtures/palettes',name+'.json'))))]) {
  const p=C.resolve(input,defaults),paint=C.radialPaint(p);
  for(const role of ['ink','muted','accent'])for(const background of [p.surface,paint.peak])
    assert(C.contrast(paint[role],background)>=4.5,role+' '+JSON.stringify(input));
  assert(C.contrast(paint.accent,paint.glyph)>=4.5);
}
for (const invalid of ['null','[]','{}','{broken','{"surface":"#GGGGGG"}',
  '{"surface":"#FFFFFF","surface":"#000000"}',
  '{"surface":"#FFFFFF","surfac\\u0065":"#000000"}',
  '{"__proto__":"#FFFFFF"}',JSON.stringify({surface:{}}),' '.repeat(8193)])
  assert.throws(()=>C.parse(invalid,defaults));
assert.equal(C.filename('Алая ночь'),'Алая ночь.json');
assert.equal(C.filename('a'.repeat(96)),'a'.repeat(96)+'.json');
assert.equal(C.filename('я'.repeat(48)),'я'.repeat(48)+'.json');
for (const name of ['','../outside','Hidden.','.Hidden',' Bad','Bad ','Bad?','CON','con.txt','LPT1','a'.repeat(97),'я'.repeat(49)])
  assert.throws(()=>C.filename(name));
const catalogInput={brand:'My colours',source:'local',license:'Own',colors:[{code:'one',name:'Test',hex:'#aabbcc'}]};
assert.equal(C.parseCatalog(JSON.stringify(catalogInput)).colors[0].hex,'#AABBCC');
assert.equal(C.parseCatalog(JSON.stringify({colors:[{hex:'#112233'}]})).colors[0].code,'1');
for(const text of ['null','[]','{}','{broken',JSON.stringify({colors:[]}),
 JSON.stringify({colors:[{hex:'#123'}]}),JSON.stringify({...catalogInput,note:{}}),
 JSON.stringify({colors:Array(10001).fill({hex:'#112233'})}),' '.repeat(2*1024*1024+1)])assert.throws(()=>C.parseCatalog(text));
assert.equal(C.parseCatalog(JSON.stringify({brand:'<img src=x>',colors:[{name:'<script>',hex:'#112233'}]})).name,'<img src=x>');
const document = fs.readFileSync(process.argv[2] || path.join(root,'build-modern/palette-editor/VitaGram Palette Editor.html'),'utf8');
const embedded=JSON.parse(document.match(/<script id="data" type="application\/json">([\s\S]*?)<\/script>/)[1]);
assert.deepEqual(Object.keys(embedded.languages).sort(),fs.readdirSync(path.join(root,'assets/lang')).filter(file=>file.endsWith('.lang')).map(file=>file.slice(0,-5)).sort());
assert.equal(Object.keys(embedded.languages).length,19);
assert.equal(Object.keys(embedded.translations).length,18);
assert.deepEqual(['ru',...Object.keys(embedded.translations)].sort(),Object.keys(embedded.languages).sort());
for(const code of ['ja','ko','zh-Hans','zh-Hant']) {
  const bytes=Buffer.from(embedded.cjk_fonts[code],'base64');
  assert.equal(bytes.subarray(0,4).toString(),'wOFF');
  assert(bytes.equals(fs.readFileSync(path.join(root,'tools/palette-editor/fonts',code+'.woff'))));
}
assert(embedded.cjk_license.includes('SIL OPEN FONT LICENSE'));
assert.equal(embedded.languages.en['settings.title'],'Settings');
assert.deepEqual(embedded.translations,translations);
assert.equal(embedded.radial.inner_radius,112);
assert.equal(embedded.radial.outer_radius,248);
assert.equal(embedded.radial.corner_radius,11);
assert.equal(embedded.presets.length,6);
assert.deepEqual(embedded.catalogs.map(c=>c.name),['Open Color','Little Greene']);
assert(embedded.catalog_license.includes('Copyright (c) 2016 heeyeun'));
assert(embedded.catalogs[0].colors.length===132);
for(const name of ['Sherwin-Williams','RAL Classic','IKEA'])assert(!document.includes(name));
const layoutSource=fs.readFileSync(path.join(root,'src/ui_layout.h'),'utf8');
for(const match of layoutSource.matchAll(/inline constexpr int (\w+) = (\d+);/g))
  assert.equal(embedded.layout[match[1]],Number(match[2]));
assert.equal(embedded.layout.header_height,36);
assert.equal(embedded.layout.chat_top_folders,151);
assert.equal(Math.floor((embedded.layout.height-embedded.layout.chat_top_folders)/embedded.layout.chat_rows_folders),43);
assert.equal(embedded.themes.length,7);
assert(embedded.themes[5].name.startsWith('black-blue'));
assert(embedded.themes[6].name.startsWith('black-red'));
assert(Buffer.from(embedded.font,'base64').equals(fs.readFileSync(path.join(root,'assets/NotoSans-Regular.ttf'))));
assert(embedded.font_license.includes('SIL OPEN FONT LICENSE'));
assert.equal(embedded.app_license,fs.readFileSync(path.join(root,'LICENSE'),'utf8'));
assert(document.includes('id="app-license"'));
assert.equal(embedded.strings['chats.tap_more'],'  |  Нажмите здесь, чтобы загрузить ещё');
assert(!document.includes('__EDITOR__')&&!document.includes('__CORE__'));
for (const entry of Object.values(embedded.manifest.assets)) for (const layer of entry.layers)
  assert.equal(Buffer.from(embedded.images[layer.mask],'base64').subarray(0,8).toString('hex'),'89504e470d0a1a0a');
console.log('Palette editor: presets, contrast, live tones, strict JSON, Unicode names, round trips and offline bundle passed');
