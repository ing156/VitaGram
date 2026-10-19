/* Russian source messages are stable keys; imported/user text is never translated. */
const StudioI18n = (() => {
  const format=(message,...values)=>message.replace(/\{(\d+)\}/g,(_,index)=>values[Number(index)]??'');
  function text(translations,language,message,...values) {
    return format((language==='ru'?message:translations[language]?.[message]??translations.en?.[message]??message),...values);
  }
  function choose(preferences,supported=['ru','en']) {
    for(const value of preferences) {
      const code=String(value).toLowerCase();
      const exact=supported.find(item=>item.toLowerCase()===code);
      if(exact)return exact;
      const base=code.split('-')[0];
      let mapped=base;
      if(base==='zh')mapped=/(?:hant|tw|hk|mo)/.test(code)?'zh-Hant':'zh-Hans';
      if(base==='pt')mapped='pt-PT';
      if(base==='nb'||base==='nn')mapped='no';
      if(supported.includes(mapped))return mapped;
    }
    return 'en';
  }
  return {format,text,choose};
})();
if(typeof module!=='undefined')module.exports=StudioI18n;
