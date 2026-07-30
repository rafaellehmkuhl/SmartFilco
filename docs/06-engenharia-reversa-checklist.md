# 06 — Engenharia reversa (checklist ao abrir a máquina)

Objetivo: documentar 100% da Philco **antes** de comprar a Leva 2 (hidráulica) e
antes de modificar. Tire **muitas fotos** de cada etapa.

> ⚠️ **Máquina desligada da tomada e sem água/pressão** antes de abrir. Capacitores
> da bomba/eletrônica podem reter carga — descarregue com cuidado.

## 0. 🔴 O parafuso do chuveiro — faça isto ANTES de tudo

É o problema **nº 1** desta máquina, disparado. Parafuso central de **alumínio**, de
fenda frágil, que prende o chuveiro na caldeira. Resíduo de café + calor fazem ele
"soldar" na rosca; ao tentar remover, a fenda espana. Vários usuários do fórum
Clube do Café tiveram que **trocar a caldeira inteira** por causa dele — o que
sozinho custa mais que boa parte da BOM deste projeto.

- [ ] **Tentar girar o parafuso do chuveiro antes de qualquer outra coisa.**
      Ferramenta certa: chave toco / chave de fenda que **preencha a fenda inteira**
      (uma moeda de 25 ou 50 centavos funciona e distribui melhor o esforço).
      Tentar com o grupo **quente** — a dilatação ajuda.
- [ ] Se não sair: **pare**. Não force até espanar. Opções, em ordem: abrir fenda
      maior com Dremel · extrator de parafusos · levar a uma tornearia para fabricar
      ferramenta ou parafuso substituto **sextavado em inox**.
- [ ] Registrar o estado dele. **Se ele não sair, isso muda o plano** — vale decidir
      entre trocar a caldeira agora (com a máquina já aberta) ou seguir sem nunca
      poder limpar o chuveiro.
- [ ] Adotar a prática preventiva: remover e limpar **periodicamente**, sempre com
      **aperto leve**, e usar puck screen para reduzir sujeira.

> Se este projeto vai manter a máquina aberta e instrumentada por anos, resolver o
> parafuso **agora** — de preferência substituindo por um de inox — é o melhor
> retorno por real gasto de toda a lista.

## A. Elétrico

- [ ] Fotografar a fiação original **antes** de desconectar qualquer coisa.
- [ ] Mapear o circuito 127/220V: onde vão os fios da **bomba**, **resistência**, **termostatos**, **fusível térmico**, **chave de vapor**, **LEDs**.
- [ ] Confirmar que o **fusível térmico** existe e onde está (será **mantido intacto**).
- [ ] **Mapear como os dois termostatos são selecionados.** Na fiação original eles
      são chaveados pela **chave de vapor**, não estão em série. O projeto precisa do
      **termostato de vapor permanentemente em série** com a resistência, como
      backstop do SSR ([07](07-seguranca-e-calibracao.md#-backstop-de-hardware-termostato-de-vapor-em-série-com-o-ssr)).
      Desenhar o circuito antes de cortar qualquer fio.
- [ ] Anotar a **temperatura de abertura** marcada em cada termostato (extração ~95 °C,
      vapor ~135–145 °C conforme o modelo) e a do fusível térmico (~165 °C).
- [ ] Confirmar que o **dimmer atual controla a bomba** (e não a resistência).
- [x] ~~Anotar tensão de trabalho~~ — **220 V**, confirmado 30/07/2026. Resistência
      de ~1000–1200 W → **4,5–5,5 A** no SSR ([07](07-seguranca-e-calibracao.md)).
- [ ] **Confirmar a potência da resistência na etiqueta** — fecha o número acima.
- [ ] **Confirmar que a bomba é a variante 230 V** (Ulka vende 120 V e 230 V). A foto
      da etiqueta da seção C responde, e disso depende a corrente calculada no dimmer
      (~0,22 A).
- [ ] Fotografar os **terminais Faston existentes** (tamanho e tipo) — serão reaproveitados.
- [ ] Desenhar o esquema elétrico original (mesmo à mão) para o arquivo.

## B. Hidráulico

- [ ] Traçar o caminho da água: reservatório → bomba → (OPV?) → boiler → grupo → porta-filtro.
- [ ] **Existe OPV? Em que pressão abre?** Único item hidráulico ainda em aberto — a
      [`03`](03-esquema-hidraulico.md#-decisão-30072026-não-comprar-opv-ajustável)
      já decidiu **não comprar** OPV ajustável; esta verificação só define se há algo
      a ajustar no que já existe.
- [x] ~~Confirmar tipo de cesta~~ — **despressurizada (naked)**, confirmado em
      30/07/2026. Importa porque com cesta pressurizada a pressão medida é dominada
      pelo restritor da cesta, não pelo puck: você estaria perfilando o acessório.
- [ ] Onde está hoje o **TEE do manômetro**; há espaço para TEE + OPV + sensor + manômetro juntos?
- [ ] Medir/confirmar **rosca 1/8" NPT** e **mangueira 4 mm** em cada conexão (macho/fêmea).
- [ ] Medir comprimentos de mangueira necessários e identificar pontos de fixação.
- [ ] Verificar se há retorno ao reservatório (para a OPV descarregar).

## C. A bomba (crítico para o controle)

- [ ] **Modelo/marca** da bomba (foto da etiqueta): é ULKA (EP5/EX5) ou clone?
- [ ] Tensão e potência (ex.: 48W / 230V).
- [ ] **Diodo: identificar pelo MODELO, não procurar na fiação.** As Ulka **EP5 e
      EX5 têm um 1N4007P integrado ao corpo da bomba** — não aparece no chicote, e
      não é escolha do fabricante da cafeteira. A foto da etiqueta acima já responde.
      - **É EP5 ou EX5** → tem diodo → trabalha em meia onda → **`divider = 2`** no
        PSM ([02](02-arquitetura-eletrica-pinout.md#-divider--2-é-a-resposta-ao-diodo-da-bomba)).
      - **É clone sem identificação** → aí sim vale medir: alimentar em baixa tensão
        AC e verificar se conduz nas duas polaridades.
- [ ] Como o dimmer atual está ligado nela.
- [ ] Registrar comportamento atual: com o dimmer, a pressão desce até ~11–12 bar mas não chega a 9 — confirmar e anotar em quais posições.

## D. Boiler / temperatura

- [ ] Material e formato do boiler (alumínio?).
- [ ] **Ponto de fixação da PT100 de superfície.** Preferência: onde ficava o
      **termostato de extração** (~95 °C) — é o ponto que o fabricante escolheu para
      representar a caldeira e já tem fixação pronta. Alternativa: sob um parafuso
      existente, com pasta térmica.
- [ ] Confirmar que sobra espaço para a sonda + roteamento do cabo de 1 m longe do AC.
- [ ] Identificar os termostatos e anotar posições (ver seção A — o de vapor **fica**).

> Não há mais busca por ponto de segundo sensor na saída do grupo — o segundo sensor
> foi **removido do projeto** ([01](01-projeto-visao-geral.md#por-que-um-sensor-de-temperatura-e-não-dois)).

## E. Mecânica / montagem

- [ ] Espaço interno para a peça 3D com os módulos: **ESP32-S3 devkit, ADS1115, level
      shifter, MAX31865, módulo microSD, SSR + dissipador, dimmer, Hi-Link**.
      (Sem display nesta fase — o recorte frontal fica para a fase de interface.)
- [ ] **Espaço e ventilação para o dissipador do SSR** — é o componente que mais
      esquenta depois do boiler.
- [ ] Ventilação/temperatura interna (afastar eletrônica do boiler).
- [ ] Ponto de **aterramento** do chassi.
- [ ] Já pensar onde caberia o display 3,5" numa fase futura, para não fechar a
      carcaça de um jeito que impeça o recorte depois.

## Saídas desta etapa

1. Esquema elétrico original documentado (foto + desenho), **incluindo o caminho dos
   dois termostatos** — é o que permite fiar o backstop com segurança.
2. Esquema hidráulico original documentado.
3. Modelo/specs da bomba confirmados → decide o método de controle ([07](07-seguranca-e-calibracao.md)).
4. Confirmação de roscas/medidas → **fecha a Leva 2** da [BOM](04-bom-lista-de-compras.md).
5. Estado do parafuso do chuveiro → decide se entra caldeira/parafuso novo na Leva 2.
6. Volume interno medido → base para modelar o **suporte 3D** dos módulos.
