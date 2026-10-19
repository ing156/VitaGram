/* Uses src/ui_layout.h, the packaged Noto font, localized strings and native PNG masks. */
function texture(name) {
  const entry=data.manifest.assets[name], key=entry.layers.map(layer=>palette[layer.role]).join('');
  if(textures.get(name)?.key===key)return textures.get(name).canvas;
  const canvas=document.createElement('canvas');[canvas.width,canvas.height]=entry.size;
  const ctx=canvas.getContext('2d');
  for(const layer of entry.layers) {
    const mask=images.get(layer.mask), plane=document.createElement('canvas');
    plane.width=mask.width;plane.height=mask.height;
    const paint=plane.getContext('2d');paint.drawImage(mask,0,0);paint.globalCompositeOperation='source-in';
    paint.fillStyle=palette[layer.role];paint.fillRect(0,0,plane.width,plane.height);
    ctx.drawImage(plane,layer.rect[0],layer.rect[1]);
  }
  textures.set(name,{key,canvas});return canvas;
}
function nine(name,x,y,w,h,opacity=1,tint=null) {
  const entry=data.manifest.assets['9slice/'+name+'.png'];let source=texture('9slice/'+name+'.png');
  if(tint) {
    const copy=document.createElement('canvas');copy.width=source.width;copy.height=source.height;
    const ctx=copy.getContext('2d');ctx.drawImage(source,0,0);const pixels=ctx.getImageData(0,0,copy.width,copy.height);
    for(let i=0;i<pixels.data.length;i+=4)for(let c=0;c<3;c++)pixels.data[i+c]=Math.floor(pixels.data[i+c]*tint[c]/255);
    ctx.putImageData(pixels,0,0);source=copy;
  }
  const [l,t,r,b]=entry.insets, sx=[0,l,source.width-r,source.width],sy=[0,t,source.height-b,source.height];
  const scaleX=Math.min(1,w/(l+r)),scaleY=Math.min(1,h/(t+b));
  const dx=[x,x+l*scaleX,x+w-r*scaleX,x+w],dy=[y,y+t*scaleY,y+h-b*scaleY,y+h];
  context.save();context.globalAlpha=opacity;
  for(let j=0;j<3;j++)for(let i=0;i<3;i++)if(dx[i+1]>dx[i]&&dy[j+1]>dy[j])
    context.drawImage(source,sx[i],sy[j],sx[i+1]-sx[i],sy[j+1]-sy[j],dx[i],dy[j],dx[i+1]-dx[i],dy[j+1]-dy[j]);
  context.restore();
}
const controllerNames=['triangle','square','cross','circle','dpad','START','SELECT','L','R'];
function tr(key,...values) {
  return (data.strings[key]||key).replace(/\{(\d+)\}/g,(_,index)=>values[Number(index)]??'')
    .replace(/\{(triangle|square|cross|circle|dpad|start|select|l|r)\}/gi,(_,name)=>
      String.fromCharCode(0xe000+controllerNames.findIndex(item=>item.toLowerCase()===name.toLowerCase())));
}
function fontScale(){return parseInt($('font-size').value,10)/100;}
function fontSize(scale){return Math.max(1,Math.floor(L.font_base*scale*fontScale()+.5));}
function font(scale){context.font=`${fontSize(scale)}px "VitaGram Noto","VitaGram CJK ${language}",sans-serif`;context.textBaseline='alphabetic';}
function controllerBounds(index) {
  const name=controllerNames[index],rect=data.manifest.assets['ps_buttons_atlas.png'].sprites[name];
  const layers=data.manifest.assets['buttons/'+name+'.png'].layers;
  const x=Math.min(...layers.map(layer=>layer.rect[0])),y=Math.min(...layers.map(layer=>layer.rect[1]));
  const right=Math.max(...layers.map(layer=>layer.rect[0]+layer.rect[2]));
  const bottom=Math.max(...layers.map(layer=>layer.rect[1]+layer.rect[3]));
  return {rect,x,y,w:right-x,h:bottom-y};
}
function textWidth(value,scale) {
  font(scale);let width=0;
  for(const run of value.split(/([\ue000-\ue008])/)) {
    if(/^[\ue000-\ue008]$/.test(run)) {const bounds=controllerBounds(run.charCodeAt(0)-0xe000);width+=bounds.w*22/bounds.h+3*scale*fontScale();}
    else width+=context.measureText(run).width;
  }
  return width;
}
function text(value,x,y,scale=.65,color=palette.text,width=Infinity) {
  if(textWidth(value,scale)>width) {
    const characters=Array.from(value);
    while(characters.length&&textWidth(characters.join('')+'...',scale)>width)characters.pop();
    value=characters.join('')+'...';
  }
  font(scale);context.fillStyle=color;
  for(const run of value.split(/([\ue000-\ue008])/)) {
    if(/^[\ue000-\ue008]$/.test(run)) {
      const bounds=controllerBounds(run.charCodeAt(0)-0xe000),factor=22/bounds.h;
      const top=y-22+(22-fontSize(scale)*.7)*.5+1.5;
      context.drawImage(texture('ps_buttons_atlas.png'),bounds.rect.x,bounds.rect.y,bounds.rect.w,bounds.rect.h,
        x-bounds.x*factor,top-bounds.y*factor,bounds.rect.w*factor,bounds.rect.h*factor);
      x+=bounds.w*factor+3*scale*fontScale();
    } else {context.fillText(run,x,y);x+=context.measureText(run).width;}
  }
}
function icon(name,x,y,size=22) {
  const rect=data.manifest.assets['ui_icons_atlas.png'].sprites[name];
  context.drawImage(texture('ui_icons_atlas.png'),rect.x,rect.y,rect.w,rect.h,x,y,size,size);
}
function clipped(x,y,w,h,draw) {
  context.save();context.beginPath();context.rect(x,y,w,h);context.clip();draw();context.restore();
}
function avatar(x,y,size,color='#7EB0D3') {
  context.fillStyle=color;context.beginPath();
  if($('avatar-shape').value==='1')context.arc(x+size/2,y+size/2,size/2,0,Math.PI*2);
  else context.roundRect(x,y,size,size,size*.22);
  context.fill();
}
function cardAlpha(focused=false) {
  return Math.min(255,Math.floor(Number($('opacity').value)*255/100)+(focused?45:0))/255;
}
function corner(right,glow=false) {
  const image=texture('corner_buttons.png'),tx=right?72:0,x=right?L.width-72:0,y=L.height-72;
  context.drawImage(image,tx,0,72,72,x,y,72,72);
  if(glow)context.drawImage(image,tx,72,72,72,x,y,72,72);
}
function header(settings=false,chat=false) {
  context.drawImage(texture('fixed/header_960x40.png'),0,0,L.width,L.header_height);
  if(chat)avatar(12,4,28,palette.selected);
  const title=settings?tr('settings.title'):chat?'Vita Community':'VitaGram '+data.version;
  const x=chat?50:20,scale=.65,y=L.header_baseline;
  text(title,x,y,scale,palette.header_text,320-x);
  const network=tr('net.online');text(network,480-textWidth(network,scale)/2,y,scale,palette.header_text,300);
  const state=Number($('battery').value),charge=['5%','25%','50%','75%','100%','75%'][state],clock='17:22';
  const clockX=948-textWidth(clock,scale)-1,separatorX=clockX-8-textWidth('|',scale),chargeX=separatorX-8-textWidth(charge,scale);
  icon(['battery_0','battery_25','battery_50','battery_75','battery_100','battery_charging'][state],chargeX-26,8,20);
  text(charge,chargeX,y,scale,palette.header_text);text('|',separatorX,y,scale,palette.header_text);
  text(clock,clockX,y,scale,palette.header_text);text(clock,clockX+1,y,scale,palette.header_text);
}
function strip(label,progress=false) {
  nine('panel',L.list_left,L.strip_top,L.list_width,L.strip_height);
  text(label,30,58,.48,palette.text,900);
  if(progress)context.drawImage(texture('fixed/active_indicator_128x4.png'),20,61,920,3);
}
function searchBar() {
  nine('row',L.list_left,L.content_top,L.list_width,L.search_height);
  icon('search',30,73,22);text(tr('search.title'),60,91,.55,palette.muted,860);
}
function drawChats() {
  searchBar();
  const tabs=[t('Все чаты'),t('Контакты'),'Vita',t('Моё'),t('Рабочие')];
  clipped(10,69+L.search_shift,940,44,()=>tabs.forEach((name,index)=>{
    const x=L.list_left+index*L.folder_step;
    nine(index===0?'tab_active':'tab_normal',x,L.content_top+L.search_shift,L.folder_width,L.folder_height);
    text(name,x+10,96+L.search_shift,.65,palette.text,164);
    if(index===0)context.drawImage(texture('fixed/active_indicator_128x4.png'),x+24,105+L.search_shift,136,4);
  }));
  const names=['Vita Community',t('VitaGram · Новости'),t('Чат Ports Collection'),t('PS Vita — вопросы'),'Old Sparky',
    t('VitaGram · Задачи'),t('Дизайн и палитры'),'ToDoVitaGram','RetroGG'];
  const pitch=Math.floor((L.height-L.chat_top_folders)/L.chat_rows_folders),inner=pitch-2;
  clipped(L.list_left,L.chat_top_folders,L.list_width,L.height-L.chat_top_folders,()=>names.forEach((name,index)=>{
    const y=L.chat_top_folders+1+index*pitch,focused=index===2;
    nine(focused?'row_focused':'row',L.list_left,y,L.list_width,inner,cardAlpha(focused));
    const size=Math.min(30,inner-12),inset=(inner-size)/2;
    avatar(L.list_left+inset,y+inset,size);
    text(name,L.chat_title_left,Math.floor(y+inner/2)+7,.72);
    if([0,1,2,3,4,6].includes(index)) {
      const cx=950-inner/2,cy=y+inner/2;
      for(let g=4;g>=1;g--) {context.fillStyle=`rgba(${C.rgb(palette.notification).join(',')},${(14+(5-g)*4)/255})`;context.beginPath();context.arc(cx,cy,6+g*2,0,Math.PI*2);context.fill();}
      context.fillStyle=palette.notification;context.beginPath();context.arc(cx,cy,6,0,Math.PI*2);context.fill();
    }
  }));
  strip(tr('chats.status',tr('chats.state_ready'),'15',tr('chats.tap_more'))+' | '+tr('chats.last_updated','17:22:35'));
}
function drawSettings() {
  const keys=['language','retention','cache_size','theme','user_colors','font_size','show_threads','batch','download_dir'];
  const values=[tr('settings.language_auto',t('Русский')),tr('settings.days','30'),tr('unit.mb','512'),$('name').value,tr('common.enabled'),
    $('font-size').value,tr('common.no'),'64','ux0:downloads/vitagram'];
  clipped(20,L.settings_top,920,L.settings_viewport,()=>{
    for(let row=0;row<9;row++) {
      const y=L.settings_top+row*L.settings_row,focused=row===3;
      nine(focused?'row_focused':'row',20,y+1,920,L.settings_row-2,cardAlpha(focused));
      text(tr('settings.'+keys[row]),38,y+29,.65,palette.text,460);
      if(row<9)text(values[row],520,y+29,.6,palette.text,row===3?376:400);
      if(row===3) {context.strokeStyle=palette.text;context.beginPath();context.moveTo(908,y+18);context.lineTo(914,y+24);context.lineTo(920,y+18);context.stroke();}
    }
  });
  const thumb=Math.max(28,L.settings_viewport*L.settings_viewport/(25*L.settings_row));
  nine('scroll_thumb',948,L.settings_top,8,thumb);
  text(tr('settings.footer'),28,528,.6);
}
function drawThemes() {
  context.fillStyle='rgba(0,0,0,'+170/255+')';context.fillRect(0,0,L.width,L.height);
  const height=L.theme_header+L.theme_visible*L.theme_row+12;
  nine('menu',L.theme_left,L.theme_top,L.theme_width,height);
  text(tr('settings.theme'),L.theme_left+28,L.theme_top+30,.8);
  const rows=[...data.themes.map(theme=>({name:theme.name,colors:C.resolve(theme.colors,defaults)})),{name:$('name').value,colors:palette}];
  const first=Math.max(0,rows.length-L.theme_visible);
  rows.slice(first).forEach((row,index)=>{
    const y=L.theme_top+L.theme_header+index*L.theme_row;
    if(index===rows.slice(first).length-1) {
      nine('menu_item_focus',L.theme_left+12,y,L.theme_width-24,L.theme_row-2);
      context.fillStyle=palette.accent;context.fillRect(L.theme_left+16,y+12,4,20);
    }
    text(row.name,L.theme_left+28,y+28,.65,palette.text,460);
    ['background','surface','accent','text','selected'].forEach((key,column)=>{
      const x=L.theme_left+510+column*32;context.fillStyle=palette.divider;context.fillRect(x-1,y+9,29,26);
      context.fillStyle=row.colors[key];context.fillRect(x,y+10,27,24);
    });
  });
  const track=L.theme_visible*L.theme_row,thumb=track*L.theme_visible/rows.length;
  if(rows.length>L.theme_visible)nine('scroll_thumb',L.theme_left+L.theme_width-9,L.theme_top+L.theme_header+track-thumb,4,thumb);
  text(tr('settings.footer'),28,528,.6,'#F0F7FF');
}
function drawDelivery(x,y,read) {
  context.strokeStyle=palette.delivery;context.lineWidth=1;
  for(let tick=0;tick<(read?2:1);tick++)for(let thick=0;thick<2;thick++) {
    const left=x+tick*7;context.beginPath();context.moveTo(left,y+6+thick);context.lineTo(left+4,y+10+thick);
    context.lineTo(left+13,y+1+thick);context.stroke();
  }
}
function drawHistory() {
  const scale=Math.max(1,fontScale()/1.2),pitch=Math.round(22*scale),grow=pitch-22;
  const day=t('17 октября 2026'),dayWidth=textWidth(day,.55)+24;
  nine('date_chip',480-dayWidth/2,L.history_top,dayWidth,22);text(day,492-dayWidth/2,L.history_top+16,.55,palette.muted);
  const messages=[{lines:[t('Привет! Посмотри новую палитру.')],time:'17:18'},
    {lines:[t('Оттенки хорошо сочетаются.'),t('Проверим рамки и читаемость текста.')],outgoing:true,time:'17:19'},
    {lines:[t('В меню темы цвета сохранятся.')],time:'17:20',focused:true},
    {lines:[t('Отправлю JSON после проверки.')],outgoing:true,time:'17:21',read:true}];
  messages[0].sender='Vita Community';
  messages[1].replyTo=messages[0];
  const quoteHeight=L.quote_padding+Math.round(L.quote_content_height*scale);
  let top=L.history_top+26;
  clipped(0,L.history_clip_top,L.width,L.height-L.history_clip_top,()=>messages.forEach(message=>{
    let width=Math.min(L.bubble_max_width,Math.max(130,...message.lines.map(line=>textWidth(line,.68)))+32);
    const reply=message.replyTo;
    if(reply)width=Math.max(width,Math.min(L.quote_max_width,Math.max(textWidth(reply.sender,.5),textWidth(reply.lines[0],.55))+54));
    const quoteSpace=reply?quoteHeight+L.quote_gap:0;
    const timeRow=20+grow+(message.lines.length-1)*pitch+19+L.time_gap,height=timeRow+L.card_pad+17+quoteSpace;
    const x=message.outgoing?L.bubble_right-width:L.bubble_left;
    const tint=message.outgoing&&!message.focused?(C.luminance(palette.background)<C.luminance(palette.text)?[170,235,200]:[205,245,215]):null;
    nine(message.focused?'row_focused':'panel',x,top,width,height-6,cardAlpha(message.focused),tint);
    let content=top+4+L.card_pad;
    if(reply) {
      const quoteX=x+10,quoteY=content-2,quoteWidth=width-20;
      nine('date_chip',quoteX,quoteY,quoteWidth,quoteHeight,235/255);
      context.fillStyle=palette.accent;context.fillRect(quoteX+6,quoteY+6,3,quoteHeight-12);
      text(reply.sender,quoteX+16,quoteY+6+Math.round(13*scale),.5,palette.accent,quoteWidth-26);
      text(reply.lines[0],quoteX+16,quoteY+quoteHeight-8,.55,palette.text,quoteWidth-26);
      content+=quoteSpace;
    }
    message.lines.forEach((line,index)=>text(line,x+16,content+20+grow+index*pitch,.68));
    const right=x+width-(message.outgoing?46:16);
    text(message.time,right-textWidth(message.time,.52),content+timeRow,.52,palette.muted);
    if(message.outgoing)drawDelivery(x+width-41,content+timeRow-11,message.read);
    top+=height;
  }));
  strip(tr('history.block_ready','100'),true);
}
function drawPopup(attach=false) {
  const items=attach?[
    ['upload.media'],['upload.file','file'],['upload.video_note_shortcut'],['upload.voice_message_shortcut'],
    ['gif.menu','search'],['sticker.title'],['reaction.title']]:[
    ['menu.open_chat'],['menu.search','search'],['menu.settings'],['menu.refresh'],['menu.prev_folder'],['menu.next_folder']];
  const x=attach?L.attach_left:L.menu_left,w=attach?L.attach_width:L.menu_width;
  const height=items.length*L.menu_row+2*L.menu_pad,top=L.menu_bottom-height,rowsTop=top+L.menu_pad;
  nine('menu',x-4,top-4,w+8,height+8);
  items.forEach(([key,glyph],index)=>{
    const row=rowsTop+index*L.menu_row,selected=index===1;
    if(selected)nine('menu_item_focus',x+L.menu_pad,row+2,w-2*L.menu_pad,L.menu_focus_height);
    if(glyph)icon(glyph,x+(attach?14:16),row+(attach?10:7),22);
    text(tr(key),x+20+(glyph?(attach?28:30):0),row+(attach?30:26),.62,palette.text,w-40-(glyph?30:0));
  });
}
function drawSearch() {
  searchBar();
  const names=['Vita Community',t('VitaGram · Новости'),'Vita Homebrew'];
  for(let index=0;index<names.length;index++) {
    const y=L.search_top+index*L.search_row;nine(index===0?'row_focused':'row',20,y,920,L.search_row-4);
    avatar(32,y+4,32);text(names[index],76,y+27,.68,palette.text,640);text(tr('search.kind_group'),800,y+26,.5,palette.muted,126);
  }
  strip(tr('search.title'));text(tr('search.footer'),84,526,.55,palette.text,780);
}
function drawPoll() {
  const day=t('17 октября 2026')+'  17:20',width=textWidth(day,.55)+28;
  nine('date_chip',480-width/2,L.history_top,width,22);text(day,494-width/2,L.history_top+16,.5,palette.muted);
  text(t('Какой цвет выбрать?'),32,102,.7);
  text(tr('poll.kind.poll')+tr('poll.anonymous')+tr('poll.single'),32,158,.52,palette.muted);
  [t('Яркий красный'),t('Электрический синий'),t('Графитовый')].forEach((label,index)=>{
    const y=L.poll_top+index*L.poll_step,checked=index===0,cx=52,cy=y+21;
    nine(checked?'row_focused':'row',L.poll_left,y,L.poll_width,L.poll_height);
    if(checked) {
      for(const [radius,alpha] of [[16,.08],[13,.16],[10,.24],[6,1]]) {
        context.fillStyle=`rgba(${C.rgb(palette.control_active).join(',')},${alpha})`;
        context.beginPath();context.arc(cx,cy,radius,0,Math.PI*2);context.fill();
      }
      context.fillStyle=`rgba(${C.rgb(palette.highlight).join(',')},${210/255})`;
      context.beginPath();context.arc(cx-1.5,cy-2,2,0,Math.PI*2);context.fill();
    }
    context.strokeStyle=checked?palette.control_active:palette.control_inactive;context.lineWidth=1;
    context.beginPath();
    for(let a=0;a<=32;a++) {
      const angle=a*2*Math.PI/32,x=cx+9*Math.cos(angle),pointY=cy+9*Math.sin(angle);
      if(a===0)context.moveTo(x,pointY);else context.lineTo(x,pointY);
    }
    context.stroke();text(label,76,y+26,.57,palette.text,832);
  });
  text(tr('poll.state.choose')+tr('poll.voters','0'),32,467,.53,palette.text,890);
  strip(tr('history.block_ready','100'),true);
}
// Static full-open wheel: same annulus, quadratic corners and color calibration as the native renderer.
function drawRadial() {
  const g=data.radial,c=C.radialPaint(palette),mix=C.nativeBlend,action=Number($('radial-action').value);
  const rgba=(color,alpha)=>`rgba(${C.rgb(color).join(',')},${Math.min(1,alpha)})`;
  const centered=(value,x,y,scale,color,width=148)=>{
    if(textWidth(value,scale)>width){const chars=Array.from(value);while(chars.length&&textWidth(chars.join('')+'...',scale)>width)chars.pop();value=chars.join('')+'...';}
    text(value,x-textWidth(value,scale)/2,y,scale,color,width);
  };
  const box=(x,y,w,h,r,color)=>{context.fillStyle=color;context.beginPath();context.roundRect(x,y,w,h,r);context.fill();};
  const tri=(a,b,d,color)=>{context.fillStyle=color;context.beginPath();context.moveTo(...a);context.lineTo(...b);context.lineTo(...d);context.closePath();context.fill();};
  const circle=(x,y,r,color)=>{context.fillStyle=color;context.beginPath();context.arc(x,y,r,0,2*Math.PI);context.fill();};
  const ring=(x,y,inner,outer,inside,outside,first=0,last=2*Math.PI)=>{
    const fill=context.createRadialGradient(x,y,inner,x,y,outer);fill.addColorStop(0,inside);fill.addColorStop(1,outside);
    context.fillStyle=fill;context.beginPath();context.arc(x,y,outer,first,last);context.arc(x,y,inner,last,first,true);context.closePath();context.fill();
  };
  const contour=(index,expansion)=>{
    const angle=(-90+index*72)*Math.PI/180,inner=g.inner_radius-expansion,outer=g.outer_radius+expansion,r=g.corner_radius+expansion;
    const gap=g.petal_gap/2-expansion,os=angle-Math.PI/5+gap/outer,oe=angle+Math.PI/5-gap/outer,is=angle-Math.PI/5+gap/inner,ie=angle+Math.PI/5-gap/inner;
    const p=(radius,a)=>[g.x+radius*Math.cos(a),g.y+radius*Math.sin(a)];
    context.beginPath();context.arc(g.x,g.y,outer,os+r/outer,oe-r/outer);
    context.quadraticCurveTo(...p(outer,oe),...p(outer-r,oe));context.lineTo(...p(inner+r,ie));
    context.quadraticCurveTo(...p(inner,ie),...p(inner,ie-r/inner));context.arc(g.x,g.y,inner,ie-r/inner,is+r/inner,true);
    context.quadraticCurveTo(...p(inner,is),...p(inner+r,is));context.lineTo(...p(outer-r,os));
    context.quadraticCurveTo(...p(outer,os),...p(outer,os+r/outer));context.closePath();
  };
  const glyph=(kind,x,y)=>{
    context.save();context.translate(x,y);
    const a=c.accent,b=c.glyph;
    if(kind===0){box(-22,-20,44,40,6,a);box(-18,-16,36,32,3,b);circle(-9,-7,4,a);tri([-17,14],[-3,-5],[12,14],a);tri([-3,14],[9,-1],[18,14],a);}
    else if(kind===1){box(-21,-20,42,40,5,a);centered('GIF',0,6,.55,b);}
    else if(kind===2){box(-17,-22,34,44,3,a);box(-13,-18,26,36,1,b);tri([3,-22],[19,-6],[3,-6],a);for(let row=0;row<3;row++)box(-9,-1+row*5,18,2,.5,a);}
    else if(kind===3){box(-8,-22,16,30,8,a);ring(0,-3,13,16,a,a,0,Math.PI);box(-16,-7,3,5,1,a);box(13,-7,3,5,1,a);box(-2,12,4,10,1,a);box(-10,21,20,3,1.5,a);}
    else {box(-21,-20,42,40,8,a);circle(-8,-5,2.7,b);circle(8,-5,2.7,b);ring(0,0,8,10,b,b,2*Math.PI*3/64,2*Math.PI*29/64);tri([7,20],[21,7],[7,7],b);}
    context.restore();
  };
  context.fillStyle='rgba(0,0,0,.094)';context.fillRect(0,0,960,544);
  ring(g.x,g.y,g.outer_radius-1,g.outer_radius+14,rgba(palette.focus,110/255),rgba(palette.focus,0));
  const titles=['upload.media','gif.menu','upload.file','radial.voice','sticker.title'];
  const details=['radial.media_detail','radial.gif_detail','radial.file_detail','radial.voice_detail','radial.sticker_detail'];
  for(let i=0;i<5;i++) {
    const focused=action===i+1,angle=(-90+i*72)*Math.PI/180,middle=(g.inner_radius+g.outer_radius)/2;
    const x=g.x+middle*Math.cos(angle),y=g.y+middle*Math.sin(angle),alpha=Math.max(217/255,cardAlpha(focused));
    for(const [expansion,a] of [[10,9],[6,19],[3,38]]){contour(i,expansion);context.fillStyle=rgba(palette.focus,a*(focused?2:1)/255);context.fill();}
    contour(i,1.1);context.fillStyle=mix(focused?palette.focus:mix(palette.border,palette.focus,.5),c.shine,focused?.25:.64);context.fill();
    const body=focused?mix(palette.surface,palette.selected,.5):palette.surface;
    const fill=context.createLinearGradient(0,g.y-g.outer_radius,0,g.y+g.outer_radius);
    fill.addColorStop(0,rgba(mix(body,c.shine,.22),alpha*.98));fill.addColorStop(1,rgba(body,alpha*.86));
    contour(i,0);context.fillStyle=fill;context.fill();
    context.save();contour(i,0);context.clip();
    const rim=context.createLinearGradient(0,g.y-g.outer_radius,0,g.y+g.outer_radius);rim.addColorStop(0,rgba(c.shine,240/255));rim.addColorStop(1,rgba(c.shine,36/255));
    contour(i,-.55);context.strokeStyle=rim;context.lineWidth=1.1;context.stroke();context.restore();
    glyph(i,x,y-25);centered(i===1?'GIF':tr(titles[i]),x,y+17,.57,c.ink);centered(tr(details[i]),x,y+36,.43,c.muted);
  }
  const r=g.inner_radius-4,focused=action===0,alpha=Math.max(245/255,cardAlpha(focused));
  ring(g.x,g.y,r,r+13,rgba(palette.focus,(focused?125:85)/255),rgba(palette.focus,0));
  ring(g.x,g.y,r-1.5,r+1.5,palette.focus,mix(palette.focus,c.shine,.4));
  const body=mix(palette.surface,c.shine,.08),glass=context.createLinearGradient(0,g.y-r,0,g.y+r);
  glass.addColorStop(0,rgba(mix(body,c.shine,.36),alpha));glass.addColorStop(.5,rgba(body,alpha));glass.addColorStop(1,rgba(body,alpha));
  circle(g.x,g.y,r-2,glass);
  centered(tr(radialReply?'radial.reply':'radial.new_message'),g.x,g.y-56,.67,c.accent,188);
  centered(tr('radial.left'),g.x-57,g.y-30,.54,c.ink);centered(tr('radial.right'),g.x+57,g.y-30,.54,c.ink);
  for(const side of [-1,1]){const x=g.x+side*85,y=g.y-36;box(x-1,y-5,2,13,1,c.accent);tri([x-5,y+4],[x+5,y+4],[x,y+10],c.accent);}
  box(g.x-94,g.y-8,188,48,7,c.accent);box(g.x-92.5,g.y-6.5,185,45,5.5,rgba(palette.surface,218/255));
  if(radialReply){centered('Vita Community',g.x,g.y+11,.5,c.ink,172);centered(t('Привет! Посмотри палитру.'),g.x,g.y+30,.43,c.muted,168);}
  else centered(tr('radial.message_hint'),g.x,g.y+22,.57,c.muted,172);
  box(g.x-16,g.y+58,32,24,4,c.accent);tri([g.x-10,g.y+80],[g.x-10,g.y+88],[g.x-2,g.y+80],c.accent);
  box(g.x-10,g.y+65,20,3,.5,c.glyph);box(g.x-10,g.y+71,14,3,.5,c.glyph);
}

function preview() {
  if(!palette)return;
  const view=$('view').value,settings=['settings','themes'].includes(view),chat=['chat','attach','poll','radial'].includes(view);
  $('radial-controls').hidden=view!=='radial';
  $('radial-mode').textContent=tr(radialReply?'radial.reply':'radial.new_message');
  context.clearRect(0,0,L.width,L.height);context.drawImage(texture('fixed/background_960x544.png'),0,0);
  header(settings,chat);
  if(settings)drawSettings();else if(view==='poll')drawPoll();else if(chat)drawHistory();else if(view==='search')drawSearch();else drawChats();
  if(view==='radial')drawRadial();
  if(view==='themes')drawThemes();
  if(view==='menu'||view==='attach')drawPopup(view==='attach');
  if(!settings) {if(chat)corner(false,view==='attach');corner(true,view==='menu');}
}
