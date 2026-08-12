// Reproduz a lógica do buffer circular da página e confere a indexação.
var NPT=8, s=new Float32Array(NPT), escI=0, nPts=0;
function at(i){return s[(escI-nPts+i+NPT*2)%NPT];}
function push(v){ s[escI]=v; escI=(escI+1)%NPT; if(nPts<NPT)nPts++; }

function lista(){var o=[];for(var i=0;i<nPts;i++)o.push(at(i));return o.join(',');}
var erros=0;
function eq(got,exp,nome){
  if(got!==exp){console.log('  ✗ '+nome+': esperado ['+exp+'] veio ['+got+']');erros++;}
  else console.log('  ✓ '+nome+' -> ['+got+']');
}

for(var k=1;k<=3;k++)push(k);
eq(lista(),'1,2,3','enchendo (3 de 8)');
for(k=4;k<=8;k++)push(k);
eq(lista(),'1,2,3,4,5,6,7,8','cheio exato');
push(9);
eq(lista(),'2,3,4,5,6,7,8,9','deslizou 1 — o mais antigo saiu');
for(k=10;k<=16;k++)push(k);
eq(lista(),'9,10,11,12,13,14,15,16','deslizou uma volta inteira');
eq(String(at(0)),'9','at(0) é sempre o mais antigo');
eq(String(at(nPts-1)),'16','at(n-1) é sempre o mais recente');

// NaN como "sem dado"
push(NaN); push(18);
var v=at(nPts-2);
eq(String(v!==v),'true','NaN sobrevive e é detectável');

console.log(erros?'\n✗ '+erros+' falha(s)':'\n✅ buffer circular correto');
process.exit(erros?1:0);
