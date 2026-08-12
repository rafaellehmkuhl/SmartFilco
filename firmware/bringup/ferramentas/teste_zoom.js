// Reproduz a lógica de zoom da página. O que se testa aqui é a INVARIANTE que
// define zoom ancorado: o índice sob o cursor tem que ficar na mesma fração da
// tela antes e depois. Errar por um índice aqui produz um gráfico que "foge"
// devagar do ponto que você está olhando — sintoma difícil de perceber e pior
// ainda de depurar no navegador.
var nPts=1000, vN=0, vOff=0, MIN_PTS=20;
function janela(){
  var n = vN>0 ? Math.min(vN,nPts) : nPts;
  var off = Math.min(vOff, Math.max(0,nPts-n));
  return [nPts-off-n, nPts-off];
}
function roda(u, paraDentro){
  var jw=janela(), ini=jw[0], fim=jw[1], n=fim-ini;
  var alvo=ini+u*n;
  var novo=Math.round(n*(paraDentro?0.8:1.25));
  novo=Math.max(MIN_PTS,Math.min(nPts,novo));
  if(novo===n) return alvo;
  var novoIni=Math.round(alvo-u*novo);
  novoIni=Math.max(0,Math.min(nPts-novo,novoIni));
  vN = novo>=nPts ? 0 : novo;
  vOff = nPts-(novoIni+novo);
  return alvo;
}
var erros=0;
function ok(c,n){ console.log((c?'  ✓ ':'  ✗ ')+n); if(!c)erros++; }

// 1) âncora: o alvo sob o cursor continua sob o cursor
var u=0.25;
for(var k=0;k<6;k++){
  var alvo=roda(u,true);
  var jw=janela(), pos=(alvo-jw[0])/(jw[1]-jw[0]);
  if(Math.abs(pos-u)>0.02){ ok(false,'âncora mantida no passo '+k+' (u='+pos.toFixed(3)+')'); break; }
  if(k===5) ok(true,'âncora mantida em 6 passos de zoom (desvio < 2%)');
}

// 2) nunca sai dos limites
vN=0;vOff=0;
for(var k=0;k<40;k++) roda(Math.random(),Math.random()<0.5);
var jw=janela();
ok(jw[0]>=0 && jw[1]<=nPts && jw[0]<jw[1], 'janela dentro de [0,nPts] após 40 rodadas aleatórias');

// 3) piso respeitado
vN=0;vOff=0;
for(var k=0;k<80;k++) roda(0.5,true);
ok(janela()[1]-janela()[0] >= MIN_PTS, 'zoom máximo respeita o piso de '+MIN_PTS+' pontos');

// 4) afastar até o fim volta a "tudo" e ao vivo
for(var k=0;k<80;k++) roda(0.5,false);
ok(vN===0, 'afastar até o fim volta a vN=0 (tudo)');

// 5) com vOff>0, cada amostra nova incrementa vOff -> a tela fica no mesmo tempo
vN=100; vOff=50;
var antes=janela();
nPts++; if(vOff>0)vOff++;
var depois=janela();
ok(antes[0]===depois[0] && antes[1]===depois[1],
   'amostra nova não escorrega a tela quando navegado para trás');

// 6) ao vivo (vOff=0) a janela ACOMPANHA o dado novo
vN=100; vOff=0;
var a2=janela(); nPts++; var d2=janela();
ok(d2[1]===a2[1]+1, 'ao vivo, a borda direita acompanha a amostra nova');

console.log(erros?'\n✗ '+erros+' falha(s)':'\n✅ zoom ancorado correto');
process.exit(erros?1:0);
