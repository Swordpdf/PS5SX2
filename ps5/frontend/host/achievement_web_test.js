// Exercise the shipped achievement UI functions without real account data (AI-assisted).
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const page = fs.readFileSync(process.argv[2], 'utf8');
const script = page.split('<script>')[1].split('</script>')[0];
new Function(script);
const nodes = new Map();
let imageQueue = [], renderedImages = [], filters = [], version = 0, timers = [];
class Element {
  constructor() { this.textContent = ''; this.html = ''; this.dataset = {}; this.classList = { add() {}, remove() {} }; this.version = version; }
  set innerHTML(value) {
    this.html = value;
    if (this === nodes.get('settings')) {
      ++version;
      renderedImages = Array.from(value.matchAll(/data-achievement-id="(\d+)"\s+data-achievement-unlocked="([01])"/g), ([,id,unlocked]) => {
        const image = new Element(); image.dataset = { achievementId: id, achievementUnlocked: unlocked }; return image;
      });
      filters = Array.from(value.matchAll(/data-achievement-filter="(\w+)"/g), ([,filter]) => {
        const button = new Element(); button.dataset.achievementFilter = filter; return button;
      });
    }
  }
  get innerHTML() { return this.html; }
  get isConnected() { return this.version === version; }
  set src(value) { this.url = value; imageQueue.push(this); }
}
const document = {
  documentElement: {},
  querySelector() { return this.getElementById("achievement-tab"); },
  getElementById(id) { if (!nodes.has(id)) nodes.set(id,new Element()); return nodes.get(id); },
  querySelectorAll(selector) { return selector === '[data-achievement-id]' ? renderedImages : filters; }
};
const context = vm.createContext({ document, console, Date, setTimeout(fn) { timers.push(fn); },
  state: { playing: { id: 'game.iso' } }, data: { id: 'game.iso' }, tab: 'achievements', token: 'fixture-token',
  achievementData: null, achievementLoading: false, achievementPage: 0, achievementFilter: 'all', achievementImageGeneration: 0,
  api: async () => context.nextSnapshot, toast() {}, select(id) { context.selected = id; }
});
vm.runInContext(script.match(/^const \$ = .*$/m)[0] + '\n' + script.match(/^const esc = .*$/m)[0],context);
vm.runInContext(script.slice(script.indexOf('const ACHIEVEMENT_ENGLISH = '), script.indexOf('function render() {')),context);
const snapshot = { game_id: 1, playing: 'game.iso', title: '<script>unsafe title</script>', message: '', entries: [] };
for (let i=1;i<=25;i++) snapshot.entries.push({ id:i,points:5,unlocked:i<=7,title:'Objective '+i,description:'Description <b>escaped</b>' });
context.achievementData = snapshot;
vm.runInContext('renderAchievements()',context);
assert(nodes.get('settings').innerHTML.includes('7 / 25 Unlocked'));
assert(nodes.get('settings').innerHTML.includes('35 / 125 points'));
assert(nodes.get('settings').innerHTML.includes('&lt;script&gt;'));
assert(!nodes.get('settings').innerHTML.includes('<script>unsafe'));
assert.equal(renderedImages.length,12);
assert.equal(imageQueue.length,2);
while (imageQueue.length) {
  const image = imageQueue.shift(); image.onload(); assert(imageQueue.length <= 2);
}
nodes.get('achievement-next').onclick();
assert.equal(renderedImages[0].dataset.achievementId,'13');
nodes.get('achievement-next').onclick();
assert.equal(renderedImages.length,1);
filters.find(b => b.dataset.achievementFilter === 'unlocked').onclick();
assert.equal(renderedImages.length,7);
assert(nodes.get('settings').innerHTML.includes('Unlocked'));
filters.find(b => b.dataset.achievementFilter === 'locked').onclick();
assert.equal(renderedImages[0].dataset.achievementId,'8');
context.state.playing = null;
vm.runInContext('renderAchievements()',context);
assert(nodes.get('settings').innerHTML.includes('Start a game'));
context.state.playing = {id:'game.iso'}; context.data.id='other.iso';
vm.runInContext('renderAchievements()',context);
assert(nodes.get('settings').innerHTML.includes('Select the running game'));
nodes.get('achievement-playing').onclick(); assert.equal(context.selected,'game.iso');
context.data.id='game.iso'; context.nextSnapshot = {...snapshot, entries: snapshot.entries.map(e => ({...e,unlocked:true}))};
(async () => {
  await vm.runInContext('refreshAchievements(true)',context);
  assert(nodes.get('settings').innerHTML.includes('25 / 25 Unlocked'));
  assert.equal(context.achievementLoading,false);
  context.achievementFilter = 'all';
  context.state.language = 'pt-BR';
  context.state.strings = {
    'achievements.title': 'Conquistas', 'achievements.unlocked': 'Desbloqueadas',
    'achievements.locked': 'Bloqueadas', 'achievements.all': 'Todas',
    'achievements.status_unlocked': 'Desbloqueada', 'achievements.status_locked': 'Bloqueada',
    'achievements.points': 'pontos', 'achievements.refresh': '<b>Atualizar</b>',
    'achievements.previous': 'Anterior', 'achievements.next': 'Próxima'
  };
  vm.runInContext('applyAchievementLanguage(); renderAchievements()',context);
  assert.equal(document.documentElement.lang,'pt-BR');
  assert.equal(nodes.get('achievement-tab').textContent,'Conquistas');
  assert(nodes.get('settings').innerHTML.includes('25 / 25 Desbloqueadas'));
  assert(nodes.get('settings').innerHTML.includes('125 / 125 pontos'));
  assert(nodes.get('settings').innerHTML.includes('Todas'));
  assert(nodes.get('settings').innerHTML.includes('Desbloqueada · 5 pontos'));
  assert(nodes.get('settings').innerHTML.includes('Anterior'));
  assert(nodes.get('settings').innerHTML.includes('Próxima'));
  assert(nodes.get('settings').innerHTML.includes('&lt;b&gt;Atualizar&lt;/b&gt;'));
  assert(!nodes.get('settings').innerHTML.includes('<b>Atualizar</b>'));
  assert.equal(vm.runInContext('achievementText("empty_filter")',context),'No achievements in this filter.');
  console.log('PASS: achievement UI escaping, totals, filters, pagination, image queue and live refresh');
})().catch(error => { console.error(error); process.exitCode=1; });
