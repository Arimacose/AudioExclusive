const fs=require('node:fs'),vm=require('node:vm');
const {getEventListeners,setMaxListeners}=require('node:events');
const source=fs.readFileSync(require('node:path').join(__dirname,'../backend-source/netease-source/ui/settings-exclusive.js'),'utf8');
const win=new EventTarget();setMaxListeners(0,win);
let timerId=0,activeIntervals=new Set(),activeTimeouts=new Set(),liveObservers=0;
const ctx=vm.createContext({window:win,document:{querySelectorAll:()=>[],documentElement:{}},
 console:{info:()=>{}},requestAnimationFrame:()=>1,
 setInterval:()=>{const id=++timerId;activeIntervals.add(id);return id;},clearInterval:id=>activeIntervals.delete(id),
 setTimeout:()=>{const id=++timerId;activeTimeouts.add(id);return id;},clearTimeout:id=>activeTimeouts.delete(id),
 MutationObserver:class{constructor(){++liveObservers;this.active=true;}observe(){}disconnect(){if(this.active){--liveObservers;this.active=false;}}}
});
for(let cycle=1;cycle<=1000;cycle++){
 vm.runInContext(source,ctx);
 if([1,10,100,1000].includes(cycle))console.log(JSON.stringify({cycle,pagehideListeners:getEventListeners(win,'pagehide').length,activeIntervals:activeIntervals.size,activeTimeouts:activeTimeouts.size,liveObservers}));
}
if(getEventListeners(win,'pagehide').length!==1)throw new Error('listener accumulation');
win.__NCExclusiveDispose();
if(getEventListeners(win,'pagehide').length!==0)throw new Error('dispose retained listener');
console.log(JSON.stringify({phase:'afterExplicitDispose',pagehideListeners:getEventListeners(win,'pagehide').length,activeIntervals:activeIntervals.size,activeTimeouts:activeTimeouts.size,liveObservers}));
win.dispatchEvent(new Event('pagehide'));
console.log(JSON.stringify({phase:'afterPagehide',pagehideListeners:getEventListeners(win,'pagehide').length,activeIntervals:activeIntervals.size,activeTimeouts:activeTimeouts.size,liveObservers}));
