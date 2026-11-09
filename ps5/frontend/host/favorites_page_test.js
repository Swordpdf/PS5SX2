// Exercise the page's actual effective-value function. (AI-assisted)
// SPDX-License-Identifier: GPL-3.0-or-later
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const html = fs.readFileSync(process.argv[2], 'utf8');
const begin = html.indexOf('function effective(item) {');
assert(begin >= 0);
const end = html.indexOf('\n}', begin) + 2;
const context = vm.createContext({data: {id: 'game', values: {}, global: {'PS5SX2/Favorite': 'true', upscale_multiplier: '3'}}});
vm.runInContext(html.slice(begin, end), context);
assert.equal(vm.runInContext('effective({key:"PS5SX2/Favorite",def:"false"}).v', context), 'false');
assert.equal(vm.runInContext('effective({key:"upscale_multiplier",def:"1"}).v', context), '3');
context.data.values['PS5SX2/Favorite'] = 'true';
assert.equal(vm.runInContext('effective({key:"PS5SX2/Favorite",def:"false"}).v', context), 'true');
context.data.values['PS5SX2/Favorite'] = 'false';
assert.equal(vm.runInContext('effective({key:"PS5SX2/Favorite",def:"false"}).v', context), 'false');
console.log('PASS: page favorites stay per-game; other settings still inherit');
