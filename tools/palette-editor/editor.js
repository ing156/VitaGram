'use strict';
const data = JSON.parse(document.getElementById('data').textContent);
const C = PaletteCore, $ = id => document.getElementById(id);
const defaults = data.manifest.palettes;
const L = data.layout;
const labels = {background:'Основной фон',surface:'Поверхности',text:'Основной текст',muted:'Вторичный текст',accent:'Акцент',
  header_top:'Шапка · верх',header_bottom:'Шапка · низ',surface_secondary:'Вторичная поверхность',hover:'Наведение',menu:'Меню',
  selected:'Выделение',focus:'Рамка выделения',divider:'Разделитель',border:'Обычная рамка',shadow:'Тени',date:'Дата',progress:'Шкала',
  green:'Успех / зарядка',header_text:'Текст шапки',highlight:'Блики',danger:'Опасность / низкий заряд',warning:'Предупреждение',
  button_cross:'Кнопка ×',button_circle:'Кнопка ○',button_square:'Кнопка □',button_triangle:'Кнопка △',
  icon:'Иконки интерфейса',notification:'Непрочитанные / свечение',control_active:'RadioButton · выбран',
  control_inactive:'RadioButton · не выбран',delivery:'Отметки доставки'};
let radialReply = false;
let palette, role = 'accent', dirty = false, borders = {};
const images = new Map(), textures = new Map();
const firstRoles=[...C.basic,'border','focus','icon','notification','control_active','control_inactive','delivery'];
const editable = [...firstRoles,...Object.keys(defaults.dark).filter(key => !firstRoles.includes(key) && !C.battery.includes(key))];
const context = $('preview').getContext('2d');

function render() {
  palette = C.adapt(palette);
  $('mode').textContent = C.luminance(palette.background) < C.luminance(palette.text) ? 'Тёмная' : 'Светлая';
  const scroll=$('roles').scrollTop;
  $('roles').replaceChildren();
  for (const key of editable) {
    const button = document.createElement('button');button.className='role'+(key===role?' selected':'');
    button.dataset.role=key;
    button.setAttribute('aria-pressed',key===role?'true':'false');
    const swatch=document.createElement('span');swatch.className='swatch';swatch.style.background=palette[key];
    const caption=document.createElement('span');caption.textContent=labels[key] || key;
    const hex=document.createElement('small');hex.textContent=palette[key];caption.append(hex);button.append(swatch,caption);
    button.onclick=()=>{role=key;render();catalog();};$('roles').append(button);
  }
  $('roles').scrollTop=scroll;
  $('role-title').textContent=labels[role];$('color').value=palette[role];$('hex').value=palette[role];
  $('pattern').replaceChildren();
  for (const key of C.basic) {
    const button=document.createElement('button');button.style.background=palette[key];button.textContent=palette[key];
    button.title=labels[key];button.setAttribute('aria-label',labels[key]+' '+palette[key]);
    button.onclick=()=>{role=key;render();catalog();};$('pattern').append(button);
  }
  const issues=C.issues(palette);$('checks').classList.toggle('warning',issues.length>0);$('checks').replaceChildren();
  if (!issues.length) $('checks').textContent='✓ Контраст текста, значков и батареи проверен.';
  else {
    $('checks').textContent='Низкий контраст — проверьте цвета перед экспортом.';
    const list=document.createElement('ul');
    for (const issue of issues) {const li=document.createElement('li');li.textContent=issue;list.append(li);}
    $('checks').append(list);
  }
  nameStatus();preview();
}
function nameStatus() {
  const count=new TextEncoder().encode($('name').value).length;
  $('name-size').textContent=`${count} / 96 байт UTF-8 · .json добавится автоматически`;
  try {C.filename($('name').value);$('name').setCustomValidity('');}
  catch(error){$('name').setCustomValidity(error.message);}
}
function setColor(value, key = role) {
  if (!/^#[\da-f]{6}$/i.test(value)) {$('status').textContent='Введите цвет в формате #RRGGBB.';return;}
  role=key;
  if (role==='border'||role==='focus') borders[role]=value.toUpperCase();
  palette={...palette,[role]:value.toUpperCase()};
  if ($('auto').checked && C.basic.includes(role)) palette=C.derive(palette,defaults,borders);
  dirty=true;$('status').textContent='Цвет обновлён. Изменения ещё не сохранены.';render();
}
function catalog() {
  const entry=data.catalogs[Number($('catalog').value)];
  if (!entry) return;
  const query=$('search').value.trim().toLocaleLowerCase();
  const filtered=entry.colors.filter(color=>[color.name,color.code,color.hex].some(value=>String(value).toLocaleLowerCase().includes(query)));
  $('catalog-colors').replaceChildren();
  $('catalog-count').textContent=`Найдено ${filtered.length} · показано ${Math.min(120,filtered.length)} · назначение: ${labels[role]}`;
  $('catalog-note').textContent=[entry.note,entry.license?'Лицензия: '+entry.license:'',entry.source?'Источник: '+entry.source:''].filter(Boolean).join(' · ');
  for (const color of filtered.slice(0,120)) {
    const button=document.createElement('button');button.className='chip';button.title=`${color.name} · ${color.code} · ${color.hex}`;
    const paint=document.createElement('span');paint.className='paint';paint.style.background=color.hex;
    const caption=document.createElement('span');caption.className='caption';caption.textContent=color.name;
    const code=document.createElement('small');code.textContent=color.code+' · '+color.hex;caption.append(code);
    button.append(paint,caption);button.onclick=()=>setColor(color.hex);$('catalog-colors').append(button);
  }
}
function choosePreset(index) {
  const preset=data.presets[index];palette=C.resolve(preset.colors,defaults);$('name').value=preset.name;
  borders={};
  dirty=false;$('status').textContent='Паттерн загружен. Настройте цвета и сохраните тему.';render();
}
async function start() {
  const face=new FontFace('VitaGram Noto','url(data:font/ttf;base64,'+data.font+')');
  document.fonts.add(await face.load());
  $('font-license').textContent=data.font_license;
  $('app-license').textContent=data.app_license;
  $('catalog-license').textContent=data.catalog_license;
  const used=new Set(Object.keys(data.manifest.assets).filter(name=>name.startsWith('9slice/')||name.startsWith('fixed/')||name.endsWith('atlas.png')||name==='corner_buttons.png'));
  const paths=new Set([...used].flatMap(name=>data.manifest.assets[name].layers.map(layer=>layer.mask)));
  await Promise.all([...paths].map(path=>new Promise((resolve,reject)=>{
    const image=new Image();image.onload=()=>{images.set(path,image);resolve();};image.onerror=()=>reject(Error('Не удалось загрузить маску: '+path));
    image.src='data:image/png;base64,'+data.images[path];
  })));
  data.presets.forEach((preset,index)=>$('preset').add(new Option(preset.name,index)));
  data.catalogs.forEach((catalog,index)=>$('catalog').add(new Option(catalog.name,index)));
  const initial=data.presets.findIndex(preset=>preset.name==='black-red');$('preset').value=String(initial<0?1:initial);
  choosePreset(Number($('preset').value));catalog();
  for(const id of ['preset','import','export','import-catalog']) $(id).disabled=false;
  $('preset').onchange=()=>{
    if (dirty && !confirm('Заменить несохранённую палитру выбранным паттерном?')) {
      $('preset').selectedIndex=-1;return;
    }
    choosePreset(Number($('preset').value));
  };
  $('color').oninput=event=>setColor(event.target.value);
  $('apply').onclick=()=>setColor($('hex').value);
  $('hex').onkeydown=event=>{if(event.key==='Enter')setColor($('hex').value);};
  $('auto').onchange=()=>{if($('auto').checked){palette=C.derive(palette,defaults,borders);dirty=true;render();}};
  $('radial-action').onchange=preview;
  for(const side of ['left','right'])$('radial-'+side).onclick=()=>{radialReply=!radialReply;preview();};
  $('preview').onclick=event=>{
    if($('view').value!=='radial')return;
    const bounds=event.target.getBoundingClientRect(),x=(event.clientX-bounds.left)*960/bounds.width-data.radial.x,y=(event.clientY-bounds.top)*544/bounds.height-data.radial.y;
    if(Math.hypot(x,y)>data.radial.outer_radius)return;
    $('radial-action').value=Math.hypot(x,y)<data.radial.inner_radius?'0':String(1+Math.floor(((Math.atan2(y,x)*180/Math.PI+126+360)%360)/72));preview();
  };
  $('view').onchange=preview;$('battery').onchange=preview;
  $('font-size').onchange=preview;$('avatar-shape').onchange=preview;
  $('opacity').oninput=()=>{$('opacity-value').textContent=$('opacity').value+'%';preview();};
  $('name').oninput=()=>{dirty=true;nameStatus();preview();};
  $('catalog').onchange=catalog;$('search').oninput=catalog;
  $('import-catalog').onclick=()=>{$('catalog-file').value='';$('catalog-file').click();};
  $('catalog-file').onchange=async()=>{
    const file=$('catalog-file').files[0];if(!file)return;
    try {
      if(file.size>2*1024*1024)throw Error('Каталог больше 2 МиБ.');
      const entry=C.parseCatalog(await file.text());
      let index=data.catalogs.findIndex(item=>item.localFile===file.name);
      if(index<0){if(data.catalogs.length>=16)throw Error('Открыто 16 каталогов; перезагрузите мастер для нового набора.');index=data.catalogs.length;}
      entry.localFile=file.name;data.catalogs[index]=entry;
      $('catalog').replaceChildren();
      data.catalogs.forEach((item,i)=>$('catalog').add(new Option(item.name+(item.localFile?' · '+item.localFile:''),i)));
      $('catalog').value=String(index);$('search').value='';catalog();
      $('status').textContent='Каталог загружен локально: '+entry.colors.length+' цветов. Выберите цвет для текущей роли.';
    } catch(error){$('status').textContent='Ошибка каталога: '+error.message;}
  };
  $('import').onclick=()=>{$('file').value='';$('file').click();};
  $('file').onchange=async()=>{
    const file=$('file').files[0];if(!file)return;
    try {
      if(file.size>8192)throw Error('JSON больше 8 КиБ.');
      const next=C.parse(await file.text(),defaults);
      if(dirty&&!confirm('Заменить несохранённую палитру импортированным файлом?'))return;
      palette=next;borders={border:next.border,focus:next.focus};$('name').value=file.name.replace(/\.json$/i,'');dirty=false;
      $('preset').selectedIndex=-1;$('status').textContent='JSON загружен. Имя файла станет названием темы на Vita.';render();
    } catch(error){$('status').textContent='Ошибка: '+error.message;}
  };
  $('export').onclick=()=>{
    try {
      const filename=C.filename($('name').value), text=C.serialize(palette);
      C.parse(text,defaults);
      const url=URL.createObjectURL(new Blob([text],{type:'application/json;charset=utf-8'}));
      const link=document.createElement('a');link.href=url;link.download=filename;link.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);dirty=false;
      $('status').textContent=`Сохранён ${filename}. Скопируйте в ux0:data/vitagram/palletes/ и перезапустите VitaGram.`;
    } catch(error){$('status').textContent='Ошибка: '+error.message;$('name').reportValidity();}
  };
  window.addEventListener('beforeunload',event=>{if(dirty){event.preventDefault();event.returnValue='';}});
}
start().catch(error=>{$('status').textContent='Ошибка запуска: '+error.message;});
