"""Confere âncoras internas das docs.

Existe porque uma reescrita de seção quebrou 7 links de uma vez sem nenhum aviso:
markdown não valida link interno, e o leitor só descobre clicando.

⚠️ A sutileza que custou tentativa e erro: o espaço que sobra de um emoji
removido **vira hífen**, não some. Por isso a âncora de um título que começa com
emoji começa com '-'. O emoji inteiro sai, seletor de variação incluído.

E o seletor de variação (U+FE0F) SOBREVIVE: '➡️' vira o emoji removido mais um
caractere invisível. Por isso dois links para o mesmo título podem parecer iguais
no terminal e um estar quebrado — foi o que aconteceu aqui, com links escritos em
sessões diferentes.
"""
import glob
import os
import re
import sys

def anc(linha):
    t = re.sub(r'^#+\s*', '', linha.rstrip('\n').lower())
    t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', t)
    t = t.replace('`', '').replace('*', '').replace('~', '')
    mantidos = [c for c in t if c.isalnum() or c in ' -_' or c == '\ufe0f']
    return re.sub(r'\s', '-', ''.join(mantidos))

# sobe até a raiz do repositório (ferramentas/ -> bringup/ -> firmware/ -> raiz)
raiz = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
os.chdir(raiz)

ruins = []
for f in sorted(glob.glob('docs/*.md') + ['README.md']):
    txt = open(f, encoding='utf-8').read()
    for alvo, frag in re.findall(r'\]\(([^)#]*)#([^)]+)\)', txt):
        cam = os.path.join(os.path.dirname(f), alvo) if alvo else f
        if not os.path.exists(cam):
            ruins.append((f, alvo, frag, 'arquivo não existe')); continue
        heads = {anc(l) for l in open(cam, encoding='utf-8') if l.startswith('#')}
        if frag not in heads:
            ruins.append((f, alvo or '(mesmo)', frag, 'sem título correspondente'))

for f, a, frag, m in ruins:
    print("  ✗ %-34s -> %s#%s  (%s)" % (os.path.basename(f), a, frag, m))
print("  %d âncora(s) quebrada(s)" % len(ruins))
sys.exit(1 if ruins else 0)
