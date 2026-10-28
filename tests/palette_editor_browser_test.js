'use strict';
// npm install --prefix /tmp/vitagram-editor-test playwright
// PLAYWRIGHT_BROWSERS_PATH=/tmp/vitagram-editor-browsers node tests/palette_editor_browser_test.js
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || '/tmp/vitagram-editor-test/node_modules/playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const root=path.resolve(__dirname,'..');
const output=process.env.EDITOR_TEST_OUTPUT || '/tmp/vitagram-editor-test';
(async()=>{
  fs.mkdirSync(output,{recursive:true});
  const browser=await chromium.launch({headless:true,args:['--no-sandbox']});
  try {
    const page=await browser.newPage({viewport:{width:1450,height:1050},deviceScaleFactor:1,locale:'ru-RU'});
    const errors=[],network=[];
    page.on('pageerror',error=>errors.push(error.message));
    page.on('request',request=>{if(/^https?:/.test(request.url()))network.push(request.url());});
    page.on('dialog',dialog=>dialog.accept());
    await page.goto(pathToFileURL(path.resolve(process.argv[2] || path.join(root,'build-modern/palette-editor/VitaGram Palette Editor.html'))).href);
    await page.waitForFunction(()=>!document.getElementById('export').disabled);
    assert.match(await page.locator('#app-license').textContent(),/MIT License/);
    assert.equal(await page.locator('#preset option').count(),6);
    assert.equal(await page.locator('#roles .role').count(),31);
    assert.equal(await page.locator('.border-settings').count(),0);
    assert.equal(await page.locator('#view option').count(),9);
    assert(await page.evaluate(()=>document.fonts.check('25px "VitaGram Noto"')));
    assert.match(await page.locator('#checks').innerText(),/Контраст.*проверен/);
    const geometry=async view=>{
      await page.selectOption('#view',view);
      return page.evaluate(()=>{
        const calls=[],corners=[],texts=[],fills=[],strokes=[],draw=window.nine,drawCorner=window.corner,drawText=window.text;
        const ctx=document.getElementById('preview').getContext('2d'),drawRect=ctx.fillRect,stroke=ctx.stroke;
        window.nine=(...args)=>{calls.push(args.slice(0,7));return draw(...args);};
        window.corner=(...args)=>{corners.push(args);return drawCorner(...args);};
        window.text=(...args)=>{texts.push(args);return drawText(...args);};
        ctx.fillRect=(...args)=>{fills.push([...args,ctx.fillStyle]);return drawRect.apply(ctx,args);};
        ctx.stroke=(...args)=>{strokes.push(ctx.strokeStyle);return stroke.apply(ctx,args);};
        try {window.preview();return {calls,corners,texts,fills,strokes,headerFont:fontSize(.65),chatFont:fontSize(.72)};}
        finally {window.nine=draw;window.corner=drawCorner;window.text=drawText;ctx.fillRect=drawRect;ctx.stroke=stroke;}
      });
    };
    const list=await geometry('list');
    const chatRows=list.calls.filter(call=>['row','row_focused'].includes(call[0])&&call[1]===10&&call[2]>=151);
    assert.equal(chatRows.length,9);
    chatRows.forEach((row,index)=>assert.deepEqual(row.slice(1,5),[10,152+index*43,940,41]));
    assert.equal(chatRows[2][0],'row_focused');
    const tabs=list.calls.filter(call=>call[0].startsWith('tab_'));assert.equal(tabs.length,5);
    tabs.forEach((tab,index)=>assert.deepEqual(tab.slice(1,5),[10+index*189,103,184,39]));
    assert(list.calls.some(call=>JSON.stringify(call.slice(0,5))===JSON.stringify(['panel',10,42,940,23])));
    assert.deepEqual(list.corners,[[true,false]]);assert.equal(list.headerFont,25);assert.equal(list.chatFont,28);
    const settings=await geometry('settings');assert.equal(settings.corners.length,0);
    assert.deepEqual(settings.calls[0].slice(0,5),['row',20,72,920,45]);
    const themes=await geometry('themes');
    assert(themes.calls.some(call=>JSON.stringify(call.slice(0,5))===JSON.stringify(['menu',120,72,720,408])));
    assert.equal(themes.calls.filter(call=>call[0]==='menu_item_focus').length,1);
    const menu=await geometry('menu');
    assert(menu.calls.some(call=>JSON.stringify(call.slice(0,5))===JSON.stringify(['menu',596,226,328,264])));
    const attach=await geometry('attach');
    assert(attach.calls.some(call=>JSON.stringify(call.slice(0,5))===JSON.stringify(['menu',36,186,308,304])));
    assert.equal(attach.corners.length,2);
    const history=await geometry('chat');
    const cards=history.calls.filter(call=>['panel','row_focused'].includes(call[0])&&call[2]>=97);
    assert.equal(cards.length,4);
    const quote=history.calls.find(call=>call[0]==='date_chip'&&call[4]===46),reply=cards[1];
    assert(quote);
    assert.deepEqual(quote.slice(1,5),[reply[1]+10,reply[2]+10,reply[3]-20,46]);
    assert.equal(quote[5],235/255);
    assert(history.fills.some(fill=>fill[0]===quote[1]+6&&fill[1]===quote[2]+6&&fill[2]===3&&fill[3]===34));
    assert(history.texts.some(run=>run[0]==='Vita Community'&&run[1]===quote[1]+16&&run[2]===quote[2]+19&&run[3]===.5));
    assert(history.texts.some(run=>run[0]==='Привет! Посмотри новую палитру.'&&run[2]===quote[2]+38&&run[3]===.55));
    assert(history.texts.some(run=>run[0]==='Оттенки хорошо сочетаются.'&&run[2]===quote[2]+72));
    assert.equal(cards[2][2],reply[2]+reply[4]+6);
    const radial=await geometry('radial');
    assert(await page.locator('#radial-controls').isVisible());
    assert(radial.texts.some(run=>run[0]==='Мультимедиа'));
    assert(radial.texts.some(run=>run[0]==='Сообщение...'));
    await page.click('#radial-left');assert.equal(await page.locator('#radial-mode').innerText(),'Ответ');
    await page.click('#radial-right');assert.equal(await page.locator('#radial-mode').innerText(),'Новое');
    const neutral=await page.locator('#preview').evaluate(canvas=>canvas.toDataURL());
    await page.selectOption('#radial-action','1');
    assert.notEqual(await page.locator('#preview').evaluate(canvas=>canvas.toDataURL()),neutral);
    await page.locator('#preview').click({position:{x:480,y:92}});
    assert.equal(await page.locator('#radial-action').inputValue(),'1');
    const search=await geometry('search');assert(!(await page.locator('#radial-controls').isVisible()));assert.deepEqual(search.calls[1].slice(0,5),['row_focused',20,103,920,40]);
    for(const [role,color] of [['notification','#F020B0'],['control_active','#FFCC22'],['control_inactive','#11CC88'],['icon','#ABCDEF'],['delivery','#FFE260']]) {
      await page.locator('[data-role="'+role+'"]').click();await page.locator('#hex').fill(color);await page.click('#apply');
    }
    await page.selectOption('#view','list');
    assert.deepEqual(await page.locator('#preview').evaluate(canvas=>Array.from(canvas.getContext('2d').getImageData(929,172,1,1).data)),[240,32,176,255]);
    const poll=await geometry('poll');
    assert(poll.strokes.includes('#ffcc22')&&poll.strokes.includes('#11cc88'));
    assert.deepEqual(poll.calls.find(call=>call[0]==='row_focused').slice(1,5),[26,190,908,48]);
    const semanticHistory=await geometry('chat');assert(semanticHistory.strokes.every(color=>color==='#ffe260'));
    await page.selectOption('#font-size','130%');
    await page.locator('#opacity').fill('50');await page.locator('#opacity').dispatchEvent('input');
    const calibrated=await geometry('list');assert.equal(calibrated.headerFont,27);
    assert.equal(calibrated.calls.find(call=>call[0]==='row'&&call[2]===152)[5],127/255);
    const largeHistory=await geometry('chat');
    assert(largeHistory.calls.some(call=>call[0]==='date_chip'&&call[4]===49));
    await page.selectOption('#font-size','120%');await page.locator('#opacity').fill('75');await page.locator('#opacity').dispatchEvent('input');
    const original=await page.locator('#preview').evaluate(canvas=>canvas.toDataURL());
    await page.selectOption('#preset',{label:'black-blue'});
    assert.notEqual(await page.locator('#preview').evaluate(canvas=>canvas.toDataURL()),original);
    await page.selectOption('#view','chat');await page.selectOption('#battery','5');
    await page.screenshot({path:path.join(output,'editor-blue.png'),fullPage:true});
    await page.locator('#preview').screenshot({path:path.join(output,'native-chat.png')});
    await page.selectOption('#view','list');
    await page.locator('#preview').screenshot({path:path.join(output,'native-list.png')});
    for(const view of ['settings','themes','menu','attach','search','poll','radial']) {
      await page.selectOption('#view',view);await page.locator('#preview').screenshot({path:path.join(output,'native-'+view+'.png')});
    }
    for(let i=0;i<6;i++) {
      await page.selectOption('#preset',String(i));
      for(const view of ['list','chat','settings','themes','menu','attach','search','poll','radial']) await page.selectOption('#view',view);
      for(let battery=0;battery<6;battery++)await page.selectOption('#battery',String(battery));
    }
    await page.selectOption('#preset',{label:'black-red'});
    for(const [role,color] of [['notification','#F020B0'],['control_active','#FFCC22'],['control_inactive','#11CC88'],['icon','#ABCDEF'],['delivery','#FFE260']]) {
      await page.locator('[data-role="'+role+'"]').click();await page.locator('#hex').fill(color);await page.click('#apply');
    }
    const beforeBorders=await page.locator('#preview').evaluate(canvas=>canvas.toDataURL());
    await page.locator('[data-role="border"]').click();await page.locator('#color').fill('#30f0b0');
    await page.locator('[data-role="focus"]').click();await page.locator('#hex').fill('#FFD54A');await page.locator('#hex').press('Enter');
    assert.equal(await page.locator('#color').inputValue(),'#ffd54a');
    assert.notEqual(await page.locator('#preview').evaluate(canvas=>canvas.toDataURL()),beforeBorders);
    await page.locator('#roles .role').filter({hasText:'Акцент'}).click();
    await page.locator('#hex').fill('#FF4568');await page.click('#apply');
    await page.locator('#auto').uncheck();await page.locator('#auto').check();
    assert.match(await page.locator('[data-role="border"]').innerText(),/#30F0B0/);
    assert.match(await page.locator('[data-role="focus"]').innerText(),/#FFD54A/);
    await page.locator('#name').fill('Алая ночь');
    await page.selectOption('#view','themes');
    await page.screenshot({path:path.join(output,'editor-red.png'),fullPage:true});
    const downloadPromise=page.waitForEvent('download');await page.click('#export');
    const download=await downloadPromise;
    assert.equal(download.suggestedFilename(),'Алая ночь.json');
    const exported=path.join(output,'exported.json');await download.saveAs(exported);
    const exportedPalette=JSON.parse(fs.readFileSync(exported,'utf8'));assert.equal(exportedPalette.accent,'#FF4568');
    assert.equal(exportedPalette.border,'#30F0B0');assert.equal(exportedPalette.focus,'#FFD54A');
    for(const [role,color] of [['notification','#F020B0'],['control_active','#FFCC22'],['control_inactive','#11CC88'],['icon','#ABCDEF'],['delivery','#FFE260']])assert.equal(exportedPalette[role],color);
    await page.locator('#file').setInputFiles(exported);
    await page.waitForFunction(()=>document.getElementById('status').textContent.startsWith('JSON загружен'));
    assert.equal(await page.locator('#name').inputValue(),'exported');
    assert.match(await page.locator('[data-role="border"]').innerText(),/#30F0B0/);
    assert.match(await page.locator('[data-role="focus"]').innerText(),/#FFD54A/);
    await page.locator('#roles .role').filter({hasText:'Акцент'}).click();
    await page.locator('#hex').fill('#FF5070');await page.click('#apply');
    assert.match(await page.locator('[data-role="border"]').innerText(),/#30F0B0/);
    assert.match(await page.locator('[data-role="focus"]').innerText(),/#FFD54A/);
    await page.locator('#file').setInputFiles({name:'Broken.json',mimeType:'application/json',buffer:Buffer.from('{broken')});
    await page.waitForFunction(()=>document.getElementById('status').textContent.startsWith('Ошибка:'));
    assert.equal(await page.locator('#name').inputValue(),'exported');
    assert.equal(await page.locator('#catalog option').count(),2);
    await page.selectOption('#catalog',{label:'Open Color'});await page.locator('#search').fill('blue-6');
    assert.equal(await page.locator('#catalog-colors .chip').count(),1);
    await page.locator('#catalog-colors .chip').click();assert.equal(await page.locator('#hex').inputValue(),'#228BE6');
    const beforeCatalog=await page.locator('#hex').inputValue();
    const imported={brand:'User library',license:'Own',colors:[{code:'mine',name:'<img src=x onerror=alert(1)>',hex:'#123abc'}]};
    await page.locator('#catalog-file').setInputFiles({name:'colours.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(imported))});
    await page.waitForFunction(()=>document.getElementById('status').textContent.startsWith('Каталог загружен локально'));
    assert.equal(await page.locator('#hex').inputValue(),beforeCatalog);
    assert.equal(await page.locator('#catalog-colors img').count(),0);
    assert.equal(await page.locator('#catalog-colors .chip').count(),1);
    await page.locator('#catalog-colors .chip').click();assert.equal(await page.locator('#hex').inputValue(),'#123ABC');
    await page.locator('#catalog-file').setInputFiles({name:'bad.json',mimeType:'application/json',buffer:Buffer.from('{"colors":[{"hex":"oops"}]}')});
    await page.waitForFunction(()=>document.getElementById('status').textContent.startsWith('Ошибка каталога'));
    assert.equal(await page.locator('#hex').inputValue(),'#123ABC');
    assert.equal(await page.locator('#catalog option').count(),3);
    imported.colors[0].hex='#456789';
    await page.locator('#catalog-file').setInputFiles({name:'colours.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(imported))});
    await page.waitForFunction(()=>document.getElementById('status').textContent.startsWith('Каталог загружен локально'));
    assert.equal(await page.locator('#catalog option').count(),3);
    assert.equal(await page.locator('#hex').inputValue(),'#123ABC');
    await page.locator('#catalog-colors .chip').click();assert.equal(await page.locator('#hex').inputValue(),'#456789');
    await page.locator('#name').fill('x'.repeat(97));await page.click('#export');
    assert.match(await page.locator('#status').innerText(),/96 байт/);
    await page.setViewportSize({width:800,height:1000});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth));
    const englishPage=await browser.newPage({locale:'en-US',viewport:{width:1450,height:1050}});
    englishPage.on('pageerror',error=>errors.push(error.message));
    englishPage.on('request',request=>{if(/^https?:/.test(request.url()))network.push(request.url());});
    await englishPage.goto(page.url());
    await englishPage.waitForFunction(()=>!document.getElementById('export').disabled);
    assert.equal(await englishPage.locator('html').getAttribute('lang'),'en');
    assert.equal(await englishPage.locator('#export').textContent(),'Save theme');
    assert.match(await englishPage.locator('[data-role="accent"]').innerText(),/Accent/);
    assert.match(await englishPage.locator('#checks').textContent(),/contrast checked/);
    await englishPage.locator('#auto').uncheck();
    await englishPage.locator('#hex').fill('#456789');await englishPage.click('#apply');
    await englishPage.locator('#name').fill('Моя палитра');
    await englishPage.locator('#catalog-file').setInputFiles({name:'Каталог.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify({brand:'Мои цвета',colors:[{name:'Синий',hex:'#123456'}]}))});
    await englishPage.waitForFunction(()=>document.getElementById('status').textContent.startsWith('Catalogue loaded locally'));
    const state=()=>englishPage.evaluate(()=>({colors:C.serialize(palette),name:$('name').value,dirty,role,catalog:$('catalog').value,catalogCount:data.catalogs.length}));
    const beforeLanguage=await state();
    for(const code of ['ru','en']) {
      await englishPage.selectOption('#language',code);
      assert.deepEqual(await state(),beforeLanguage);
      assert.equal(await englishPage.locator('html').getAttribute('lang'),code);
      assert.match(await englishPage.locator('#catalog option:checked').textContent(),/Мои цвета/);
    }
    await englishPage.locator('#file').setInputFiles({name:'bad.json',mimeType:'application/json',buffer:Buffer.from('{bad')});
    await englishPage.waitForFunction(()=>document.getElementById('status').textContent==='Error: Invalid JSON.');
    const captions=await englishPage.evaluate(()=>{
      const original=window.text,seen=[];window.text=(...args)=>{seen.push(args[0]);return original(...args);};
      for(const option of $('view').options){$('view').value=option.value;preview();}
      window.text=original;return seen;
    });
    assert(captions.includes('Settings')&&captions.includes('Online')&&captions.includes('All chats'));
    assert(!captions.some(value=>/[А-Яа-яЁё]/.test(value)&&!['Моя палитра','Мои цвета'].includes(value)));
    const downloadEvent=englishPage.waitForEvent('download');await englishPage.click('#export');
    const englishDownload=await downloadEvent;assert.equal(englishDownload.suggestedFilename(),'Моя палитра.json');
    const englishExport=path.join(output,'english-export.json');await englishDownload.saveAs(englishExport);
    assert.equal(fs.readFileSync(englishExport,'utf8'),beforeLanguage.colors);
    assert.equal(await englishPage.locator('#language option').count(),19);
    const savedState=await state();
    await englishPage.setViewportSize({width:800,height:1000});
    for(const code of await englishPage.locator('#language option').evaluateAll(options=>options.map(option=>option.value))) {
      await englishPage.selectOption('#language',code);
      assert.deepEqual(await state(),savedState);
      assert(await englishPage.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth),code);
      const localized=await englishPage.evaluate(()=>{
        const expected=message=>language==='ru'?message:data.translations[language][message];
        for(const element of document.querySelectorAll('[data-i18n]')) {
          if(['status','radial-mode'].includes(element.id))continue;
          if(element.textContent!==expected(element.dataset.i18n))return false;
        }
        for(const option of $('view').options){$('view').value=option.value;preview();}
        return $('export').textContent===t('Сохранить тему')&&C.serialize(palette).length>0;
      });
      assert(localized,code);
    }
    await englishPage.selectOption('#language','ja');
    assert(await englishPage.evaluate(()=>document.fonts.check('25px "VitaGram CJK ja"')));
    await englishPage.screenshot({path:path.join(output,'editor-i18n-ja.png'),fullPage:true});
    await englishPage.selectOption('#language','ru');await englishPage.reload();
    await englishPage.waitForFunction(()=>!document.getElementById('export').disabled);
    assert.equal(await englishPage.locator('html').getAttribute('lang'),'ru');
    assert.equal(await englishPage.locator('#export').textContent(),'Сохранить тему');
    await englishPage.screenshot({path:path.join(output,'editor-i18n-ru.png'),fullPage:true});
    await englishPage.selectOption('#language','en');
    await englishPage.screenshot({path:path.join(output,'editor-i18n-en.png'),fullPage:true});
    await englishPage.close();
    const blocked=await browser.newPage({locale:'fr-FR'});
    blocked.on('pageerror',error=>errors.push(error.message));
    await blocked.addInitScript(()=>Object.defineProperty(window,'localStorage',{get(){throw Error('Storage blocked');}}));
    await blocked.goto(page.url());await blocked.waitForFunction(()=>!document.getElementById('export').disabled);
    assert.equal(await blocked.locator('html').getAttribute('lang'),'fr');
    await blocked.selectOption('#language','zh-Hant');
    assert.equal(await blocked.locator('#export').textContent(),'儲存主題');
    await blocked.close();
    assert.deepEqual(errors,[]);assert.deepEqual(network,[]);
    console.log('Browser: 6 patterns, 9 native views, notification/radio/delivery colors, unified colors, JSON, private catalogue imports, 19 locales, state preservation and zero network passed');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
