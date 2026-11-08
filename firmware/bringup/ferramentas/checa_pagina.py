"""Checagem estática da index.html antes de gravar.

Existe por causa de um bug real: `histCache` foi usado e nunca declarado, a
inicialização lançou ReferenceError e a página subiu SEM DADOS. `node --check`
não pega isso — ele valida sintaxe, e a expressão era sintaticamente perfeita.

Confere três coisas que já quebraram nesta bancada:
  1. identificador atribuído/lido no topo e nunca declarado
  2. id referenciado no JS que não existe no HTML
  3. chave de tradução sem par entre os dois idiomas
"""

import re
import sys

CAM = sys.argv[1] if len(sys.argv) > 1 else "firmware/bringup/main/index.html"
s = open(CAM, encoding="utf-8").read()
i = s.rfind("<script>")
html, js_bruto = s[:i], s[i + 8:s.rfind("</script>")]


def limpa(t):
    """Tira comentários e literais de texto — sem isso o checador lê prosa."""
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"//[^\n]*", " ", t)
    t = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", t)
    t = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', t)
    t = re.sub(r"`(?:[^`\\]|\\.)*`", "``", t)
    # ⚠️ Regex literal só é regex se vier em posição de INÍCIO DE EXPRESSÃO. Sem
    # esta guarda, `lo=1/0,hi=-1/0` é lido como a regex `/0,hi=-1/` e some com a
    # declaração de `hi` — foi o primeiro falso positivo deste checador.
    t = re.sub(r"(?<=[(,=:\[!&|?{;])\s*/(?:[^/\\\n\[]|\\.|\[[^\]]*\])+/[gimsuy]*",
               " RE ", t)
    return t


js = limpa(js_bruto)

# declarações: `var a=1, b, c=3;` declara os três
declarados = set()
for m in re.finditer(r"\bvar\s+([^;]*)", js):
    corpo, prof = m.group(1), 0
    for parte in re.split(r",(?![^(\[]*[)\]])", corpo):
        n = re.match(r"\s*([A-Za-z_$][\w$]*)", parte)
        if n:
            declarados.add(n.group(1))
declarados |= set(re.findall(r"\bfunction\s+([A-Za-z_$][\w$]*)", js))
for p in re.findall(r"function\s*[A-Za-z_$\w]*\s*\(([^)]*)\)", js):
    declarados |= {x.strip() for x in p.split(",") if x.strip()}

ids = set(re.findall(r'id="([A-Za-z0-9_]+)"', html))
GLOBAIS = set("""window document console localStorage location fetch JSON Math Date
String Number Array Object parseFloat parseInt isNaN isFinite setTimeout WebSocket
performance Blob requestAnimationFrame getComputedStyle devicePixelRatio innerWidth innerHeight addEventListener Promise
encodeURIComponent getComputedStyle Event true false null undefined NaN this
Float32Array Float64Array Int16Array Uint8Array Uint16Array Int32Array Uint32Array
typeof new return if else for while var function break continue in of try catch
throw delete void""".split())

# só o que é ATRIBUÍDO ou CHAMADO — é onde o ReferenceError aparece
alvos = set(re.findall(r"(?<![.\w$])([A-Za-z_$][\w$]*)\s*(?:=[^=]|\()", js))
faltando = sorted(a for a in alvos if a not in declarados and a not in ids and a not in GLOBAIS)

en = js_bruto[js_bruto.find("en:{"):js_bruto.find("pt:{")]
pt = js_bruto[js_bruto.find("pt:{"):js_bruto.find("}};")]
ken = set(re.findall(r"([a-z][a-z0-9_]*)\s*:\s*'", en))
kpt = set(re.findall(r"([a-z][a-z0-9_]*)\s*:\s*'", pt))
usadas = set(re.findall(r'data-i18n="([a-z0-9_]+)"', html))
usadas |= set(re.findall(r'data-i18n-t="([a-z0-9_]+)"', html))
usadas |= set(re.findall(r"\bt\('([a-z0-9_]+)'\)", js_bruto))

erros = 0
if faltando:
    print("✗ atribuído/chamado e nunca declarado:", faltando); erros += 1
else:
    print("✓ todo identificador tem declaração")

sem_id = sorted(set(
    u for u in re.findall(r"(?<![.\w$])([a-zA-Z][\w$]*)\.(?:textContent|innerHTML|"
                          r"classList|dataset|hidden|disabled|checked)", js)
    if u not in ids and u not in declarados and u not in GLOBAIS))
if sem_id:
    print("✗ ids do JS ausentes no HTML:", sem_id); erros += 1
else:
    print("✓ todo id usado existe no HTML")

# id e nome declarado iguais: o id vira global no window, mas `var`/`function`
# de mesmo nome o SOBRESCREVE — e o código que achava estar falando com o
# elemento fala com a função. Foi assim que `trilhas` (função do LED) quebrou a
# página inteira na inicialização, com WebSocket e tudo, em 08/11/2026.
nomes = set(re.findall(r"\bfunction\s+([A-Za-z_$][\w$]*)", js))
nomes |= set(re.findall(r"^\s*var\s+([A-Za-z_$][\w$]*)", js, flags=re.M))
colide = sorted(nomes & ids)
if colide:
    print("✗ id do HTML com o mesmo nome de var/function:", colide); erros += 1
else:
    print("✓ nenhum id colide com nome declarado")

if usadas - ken or ken ^ kpt:
    print("✗ tradução: sem chave", sorted(usadas - ken), "| descasadas", sorted(ken ^ kpt))
    erros += 1
else:
    print("✓ %d chaves de tradução, EN e PT completos" % len(ken))

sys.exit(1 if erros else 0)
