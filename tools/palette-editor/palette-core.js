/* Shared by the offline editor and Node checks. No network or dependencies. */
const PaletteCore = (() => {
  const basic = ['background', 'surface', 'text', 'muted', 'accent'];
  const battery = ['battery_low', 'battery_warning', 'battery_charging'];
  const rgb = color => color.slice(1).match(/../g).map(value => parseInt(value, 16));
  const hex = values => '#' + values.map(value => value.toString(16).padStart(2, '0')).join('').toUpperCase();
  const blend = (a, b, amount) => hex(rgb(a).map((value, index) => Math.round(value * (1 - amount) + rgb(b)[index] * amount)));
  function luminance(color) {
    return rgb(color).reduce((sum, value, index) => {
      value /= 255;
      return sum + [0.2126, 0.7152, 0.0722][index] * (value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4);
    }, 0);
  }
  function contrast(a, b) {
    const values = [luminance(a), luminance(b)].sort((a, b) => a - b);
    return (values[1] + .05) / (values[0] + .05);
  }
  function tone(color, top, bottom, minimum = 3) {
    const score = color => Math.min(contrast(color, top), contrast(color, bottom));
    if (score(color) >= minimum) return color;
    const target = score('#000000') > score('#FFFFFF') ? '#000000' : '#FFFFFF';
    for (let step = 1; step <= 32; step++) {
      const candidate = hex(rgb(color).map((value, index) => Math.floor((value * (32 - step) + rgb(target)[index] * step) / 32)));
      if (score(candidate) >= minimum) return candidate;
    }
    return target;
  }
  const nativeBlend = (a,b,amount) => hex(rgb(a).map((value,index)=>Math.floor(value*(1-amount)+rgb(b)[index]*amount)));
  const on = background => luminance(background) > .179 ? '#000000' : '#FFFFFF';
  function radialPaint(p) {
    const shine=luminance(p.surface)<luminance(p.text)?nativeBlend(p.surface,p.text,.14):'#FFFFFF';
    const peak=nativeBlend(nativeBlend(p.surface,p.selected,.5),shine,.22);
    const accent=tone(p.accent,p.surface,peak,4.5);
    return {shine,peak,ink:tone(p.text,p.surface,peak,4.5),muted:tone(p.muted,p.surface,peak,4.5),accent,glyph:on(accent)};
  }
  function adapt(palette) {
    const p = {...palette};
    p.header_text = tone(p.header_text, p.header_top, p.header_bottom);
    for (const [role, source] of [['battery_low', 'danger'], ['battery_warning', 'warning'], ['battery_charging', 'green']])
      p[role] = tone(p[source], p.header_top, p.header_bottom);
    return p;
  }
  function resolve(input, defaults) {
    const mode = luminance(input.background) < luminance(input.text) ? 'dark' : 'light';
    const result={...defaults[mode],...input};
    for(const [role,source] of [['icon','text'],['control_active','accent'],['control_inactive','muted'],['delivery','green']])
      if(!Object.hasOwn(input,role))result[role]=result[source];
    return adapt(result);
  }
  function derive(input, defaults, borders = {}) {
    const p = {...input};
    Object.assign(p, {
      header_top:p.accent, header_bottom:blend(p.accent,p.background,.1),
      surface_secondary:blend(p.surface,p.accent,.08), hover:blend(p.surface,p.accent,.12),
      menu:p.surface, selected:blend(p.surface,p.accent,.18), focus:p.accent,
      divider:blend(p.surface,p.text,.18), border:blend(p.surface,p.text,.28),
      shadow:blend(p.background,'#000000',.65), date:blend(p.surface,p.accent,.08), progress:blend(p.surface,p.text,.18)
    });
    const backgrounds = ['header_top', 'header_bottom'];
    p.header_text = [p.text,p.background,p.surface].reduce((best, candidate) =>
      Math.min(...backgrounds.map(key => contrast(candidate,p[key]))) > Math.min(...backgrounds.map(key => contrast(best,p[key]))) ? candidate : best);
    for (const key of ['border','focus']) if (Object.hasOwn(borders,key)) p[key] = borders[key];
    return resolve(p, defaults);
  }
  function parse(text, defaults) {
    if (new TextEncoder().encode(text).length > 8192) throw Error('JSON больше 8 КиБ.');
    const input = JSON.parse(text);
    if (!input || Array.isArray(input) || typeof input !== 'object') throw Error('Нужен JSON-объект цветов.');
    const seen = new Set();
    for (const match of text.matchAll(/"((?:[^"\\]|\\.)*)"\s*:/g)) {
      const key = JSON.parse('"' + match[1] + '"');
      if (seen.has(key)) throw Error('Повторяющаяся роль: ' + key);
      seen.add(key);
    }
    for (const [key,value] of Object.entries(input)) {
      if (!Object.hasOwn(defaults.dark,key)) throw Error('Неизвестная роль: ' + key);
      if (typeof value !== 'string' || !/^#[\da-f]{6}$/i.test(value)) throw Error(key + ': нужен цвет #RRGGBB.');
      input[key] = value.toUpperCase();
    }
    for (const key of basic) if (!Object.hasOwn(input,key)) throw Error('Отсутствует роль: ' + key);
    return resolve(input, defaults);
  }
  function parseCatalog(text) {
    if (new TextEncoder().encode(text).length>2*1024*1024) throw Error('Каталог больше 2 МиБ.');
    const input=JSON.parse(text);
    if (!input || Array.isArray(input) || typeof input!=='object' || !Array.isArray(input.colors) ||
        !input.colors.length || input.colors.length>10000) throw Error('Нужен объект с массивом colors: от 1 до 10000 цветов.');
    const field=(value,fallback,max)=>{
      if(value===undefined)return fallback;
      if(typeof value!=='string' || value.length>max)throw Error('Некорректное текстовое поле каталога.');
      return value;
    };
    const colors=input.colors.map((color,index)=>{
      if(!color || typeof color!=='object' || Array.isArray(color) || typeof color.hex!=='string' ||
         !/^#[\da-f]{6}$/i.test(color.hex))throw Error('Цвет '+(index+1)+': нужен hex #RRGGBB.');
      const code=field(color.code, String(index+1),128);
      return {code,name:field(color.name,code,256),hex:color.hex.toUpperCase()};
    });
    const name=field(input.brand,field(input.name,'Мой каталог',128),128).trim() || 'Мой каталог';
    return {name,colors,note:field(input.note,'Локальный каталог пользователя.',4096),
      license:field(input.license,'',512),source:field(input.source,'',2048)};
  }
  function filename(name) {
    if (!name || name.startsWith('.') || name !== name.trim() || name.endsWith('.') || /[<>:"/\\|?*\x00-\x1f\x7f]/.test(name))
      throw Error('Название не должно содержать служебные символы, крайние пробелы или точки.');
    if (new TextEncoder().encode(name).length > 96) throw Error('Название длиннее 96 байт UTF-8. Сократите его.');
    if (/^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)/i.test(name)) throw Error('Это имя зарезервировано Windows.');
    return name + '.json';
  }
  function issues(p) {
    const result = [];
    for (const ink of ['text','muted']) for (const back of ['background','surface','surface_secondary','hover','selected','menu','date']) {
      const ratio = contrast(p[ink],p[back]);
      if (ratio < 4.5) result.push(`${ink} / ${back}: ${ratio.toFixed(2)}:1 (нужно 4.5:1)`);
    }
    for (const ink of ['header_text',...battery]) for (const back of ['header_top','header_bottom']) {
      const ratio = contrast(p[ink],p[back]);
      if (ratio < 3) result.push(`${ink} / ${back}: ${ratio.toFixed(2)}:1 (нужно 3:1)`);
    }
    if (contrast(p.accent,p.surface) < 3) result.push('Акцент сливается с поверхностью: нужно 3:1.');
    return result;
  }
  function serialize(p) {
    return JSON.stringify(Object.fromEntries(Object.entries(p).filter(([role]) => !battery.includes(role))),null,2) + '\n';
  }
  return {parseCatalog,nativeBlend,on,radialPaint,basic,battery,rgb,hex,blend,luminance,contrast,tone,adapt,resolve,derive,parse,filename,issues,serialize};
})();
if (typeof module !== 'undefined') module.exports = PaletteCore;
