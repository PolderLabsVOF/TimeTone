// These interactions simulate device state only; no hardware requests are made.
const screen = document.querySelector('#screen');
const params = new URLSearchParams(location.search);
const states = {online:'Connected',offline:'Offline',connecting:'Connecting',syncing:'Syncing',retrying:'Sync failed; retrying'};
const chevron = '<svg class="chevron" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="m6 4 4 4-4 4"/></svg>';
const check = '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="m4 10 4 4 8-8"/></svg>';
let connection = states[params.get('state')] ? params.get('state') : 'online';
function setConnection(state) {
  connection = state;
  const dot = document.querySelector('.dot');
  dot.dataset.state = state;
  dot.setAttribute('aria-label', states[state] + ' (simulated)');
  dot.title = states[state] + ' (simulated)';
}
setConnection(connection);
if(params.has('embed')) document.body.classList.add('embedded');
const boardLink = document.createElement('a');
boardLink.href='terminal-design-board.html';boardLink.className='board-link';boardLink.textContent='Component board';
document.querySelector('.preview').append(boardLink);
const picker = document.createElement('section');
picker.id='picker';picker.className='page';picker.hidden=true;
picker.innerHTML='<header class="header"><strong class="brand" id="picker-title"></strong><button id="picker-back">Back</button></header><div class="scroll picker-list" role="radiogroup" aria-labelledby="picker-title"></div>';
screen.append(picker);pages.push('picker');
const baseShow = show;
show = function(id) {
  const previous = pages.find(page=>!document.getElementById(page).hidden);
  baseShow(id);
  pages.forEach(page=>document.getElementById(page).classList.remove('enter','return'));
  if(previous!==id) document.getElementById(id).classList.add(id==='keypad'||previous==='picker'?'return':'enter');
};
let activeSelect, activeTrigger;
const selectTriggers = new Map();
function updateValues(){
  selectTriggers.forEach((button,select)=>button.querySelector('.picker-value span').textContent=select.selectedOptions[0].textContent);
}
function closePicker(){
  if(activeTrigger) activeTrigger.setAttribute('aria-expanded','false');
  show('settings');activeTrigger?.focus({preventScroll:true});
}
function openPicker(select,trigger){
  activeSelect=select;activeTrigger=trigger;
  trigger.setAttribute('aria-expanded','true');
  document.querySelector('#picker-title').textContent=select.getAttribute('aria-label');
  const list=picker.querySelector('.picker-list');list.replaceChildren();
  Array.from(select.options).forEach(option=>{
    const button=document.createElement('button');
    button.className='choice';button.setAttribute('role','radio');
    button.setAttribute('aria-checked',String(option.selected));
    const label=document.createElement('span');label.textContent=option.textContent;button.append(label);
    if(option.selected)button.insertAdjacentHTML('beforeend',check);
    button.onclick=()=>{
      select.value=option.value;select.dispatchEvent(new Event('change',{bubbles:true}));
      updateValues();closePicker();
    };
    list.append(button);
  });
  show('picker');list.querySelector('[aria-checked=true]').focus();
}
document.querySelectorAll('.row select').forEach(select=>{
  const label=select.closest('label');
  const trigger=document.createElement('button');
  trigger.className='row';trigger.type='button';
  trigger.setAttribute('aria-expanded','false');trigger.setAttribute('aria-controls','picker');
  trigger.innerHTML='<span></span><span class="picker-value"><span></span>'+chevron+'</span>';
  trigger.firstElementChild.textContent=select.getAttribute('aria-label');
  label.before(trigger);label.hidden=true;
  selectTriggers.set(select,trigger);
  trigger.onclick=()=>openPicker(select,trigger);
});
updateValues();
document.querySelector('#picker-back').onclick=closePicker;
picker.addEventListener('keydown',event=>{
  const options=Array.from(picker.querySelectorAll('.choice'));
  const index=options.indexOf(document.activeElement);
  if(index<0)return;
  let next;
  if(event.key==='ArrowDown')next=(index+1)%options.length;
  if(event.key==='ArrowUp')next=(index+options.length-1)%options.length;
  if(event.key==='Home')next=0;
  if(event.key==='End')next=options.length-1;
  if(next!==undefined){event.preventDefault();options[next].focus();}
});
const motion=document.createElement('button');
motion.className='row';motion.setAttribute('role','switch');motion.setAttribute('aria-checked','false');
motion.innerHTML='<span>Reduce motion</span><span class="switch-track" aria-hidden="true"></span>';
motion.onclick=()=>{const reduced=motion.getAttribute('aria-checked')!=='true';motion.setAttribute('aria-checked',String(reduced));screen.classList.toggle('reduced',reduced);};
document.querySelector('.themes').after(motion);
let syncTimer;
document.querySelector('#sync').onclick=()=>{
  const button=document.querySelector('#sync'),label=document.querySelector('#sync-status');
  const fails=connection==='offline'||connection==='retrying';
  button.disabled=true;label.textContent='Syncing…';setConnection('syncing');
  clearTimeout(syncTimer);
  syncTimer=setTimeout(()=>{
    setConnection(fails?'retrying':'online');label.textContent=fails?'Retry':'Done';
    button.disabled=false;
  },1200);
};
document.addEventListener('keydown',event=>{
  if(event.key!=='Escape')return;
  event.stopImmediatePropagation();
  if(!picker.hidden)closePicker();
  else if(!document.querySelector('#calibration').hidden){show('settings');document.querySelector('#calibrate').focus({preventScroll:true});}
  else{show('keypad');document.querySelector('#open').focus();}
},true);
if(params.get('theme')==='dark')document.querySelector('[data-theme=dark]').click();
if(params.get('motion')==='reduced')motion.click();
if(params.get('view')==='settings')show('settings');
if(params.get('view')==='picker')openPicker(document.querySelector('#screen-off'),selectTriggers.get(document.querySelector('#screen-off')));
if(params.get('view')==='calibration')document.querySelector('#calibrate').click();
if(params.get('view')==='connection'){show('settings');document.querySelector('#sync').scrollIntoView({block:'center'});}
if(params.get('view')==='entry'){count=2;render();document.querySelector('#feedback').textContent='2 of 4';}
