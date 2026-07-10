# 06 — Engenharia reversa (checklist ao abrir a máquina)

Objetivo: documentar 100% da Philco **antes** de comprar a Leva 2 (hidráulica) e
antes de modificar. Tire **muitas fotos** de cada etapa.

> ⚠️ **Máquina desligada da tomada e sem água/pressão** antes de abrir. Capacitores
> da bomba/eletrônica podem reter carga — descarregue com cuidado.

## A. Elétrico

- [ ] Fotografar a fiação original **antes** de desconectar qualquer coisa.
- [ ] Mapear o circuito 127/220V: onde vão os fios da **bomba**, **resistência**, **termostatos**, **fusível térmico**, **chave de vapor**, **LEDs**.
- [ ] Confirmar que o **fusível térmico** existe e onde está (será **mantido**).
- [ ] Confirmar que o **dimmer atual controla a bomba** (e não a resistência).
- [ ] Anotar tensão de trabalho (127 ou 220V) e potência da resistência (etiqueta).
- [ ] Desenhar o esquema elétrico original (mesmo à mão) para o arquivo.

## B. Hidráulico

- [ ] Traçar o caminho da água: reservatório → bomba → (OPV?) → boiler → grupo → porta-filtro.
- [ ] **Existe OPV?** Se sim, onde e qual a pressão de abertura (se marcada/ajustável).
- [ ] Onde está hoje o **TEE do manômetro**; há espaço para TEE + OPV + sensor + manômetro juntos?
- [ ] Medir/confirmar **rosca 1/8" NPT** e **mangueira 4 mm** em cada conexão (macho/fêmea).
- [ ] Medir comprimentos de mangueira necessários e identificar pontos de fixação.
- [ ] Verificar se há retorno ao reservatório (para a OPV descarregar).

## C. A bomba (crítico para o controle)

- [ ] **Modelo/marca** da bomba (foto da etiqueta): é ULKA (EP5/EX5) ou clone?
- [ ] Tensão e potência (ex.: 48W / 230V).
- [ ] Como o dimmer atual está ligado nela.
- [ ] Registrar comportamento atual: com o dimmer, a pressão desce até ~11–12 bar mas não chega a 9 — confirmar e anotar em quais posições.

## D. Boiler / temperatura

- [ ] Material e formato do boiler (alumínio?), onde há espaço para rosca/fixação da **PT1000 #1**.
- [ ] Identificar termostatos (café ~95°C / vapor ~140°C) — serão substituídos pelo controle digital, mas anotar posições.
- [ ] Melhor ponto para a **PT1000 #2** na saída do grupo.

## E. Mecânica / montagem

- [ ] Espaço interno disponível para a peça 3D com os módulos (ESP32+display, ADS1115, 2× MAX31865, SSR, dimmer, Hi-Link).
- [ ] Onde posicionar o **display touch** na carcaça (recorte frontal?).
- [ ] Ventilação/temperatura interna (afastar eletrônica do boiler).
- [ ] Ponto de **aterramento** do chassi.

## Saídas desta etapa

1. Esquema elétrico original documentado (foto + desenho).
2. Esquema hidráulico original documentado.
3. Modelo/specs da bomba confirmados → decide o método de controle ([07](07-seguranca-e-calibracao.md)).
4. Confirmação de roscas/medidas → **fecha a Leva 2** da [BOM](04-bom-lista-de-compras.md).
5. Volume interno medido → base para modelar o **suporte 3D** dos módulos.
