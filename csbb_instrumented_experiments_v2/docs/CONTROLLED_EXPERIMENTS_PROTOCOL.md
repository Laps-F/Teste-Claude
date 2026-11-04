# Protocolo de comparação controlada para B&B MOSP (v5)

## Objetivo

Separar o impacto de cada mecanismo selecionável do solver, usando quatro instâncias reais da família Frinhani–Carvalho–Soma (uma 400×400, 600×600, 800×800, 1000×1000) e uma matriz de experimentos pareados. Os scripts não substituem uma campanha de múltiplas instâncias por dimensão.

## Execução

```bash
./scripts/build.sh
python3 scripts/controlled_experiments.py --list-comparisons
python3 scripts/controlled_experiments.py --suite core --threads auto --reserve-cores 1 --time-limit 3600 --repeats 3 --output-dir results/controlled_1h
```

O conjunto `core` inclui comparação de warm start, bound por grafo, residual next/core, root probe, relaxação full serial, customer paralelo, paralelismo da prova relaxada, branching close-stack e paralelismo close-stack. `extended` acrescenta MCNh preprocessing, hash, move sorting, move grouping, reverse-anchor e política de desfusão. Use `--comparisons id1,id2` para uma seleção específica e `--dry-run` para ver todos os comandos sem executar. Para retomar: `--resume` com parâmetros e binários idênticos. Reprocessar somente o relatório: `python3 scripts/controlled_report.py --output-dir results/controlled_1h`.

## Controles e isolamento

- Instâncias originais: `Random-{400,600,800,1000}-{400,600,800,1000}-2-1.txt`, primeiras de cada dimensão, com hash e dimensões validados.
- O limite de tempo é idêntico e aplicado **individualmente** a todas as execuções do B&B; preparação do MCNh ocorre uma única vez por instância/modo de preprocessing, fora do tempo do B&B e é registrada separadamente.
- Para cada variante que usa warm start, o mesmo arquivo MCNh com sequência original de produtos é passado com `--warm-start-policy strict`, validado internamente pelo B&B. Rejeição do certificado invalida a execução.
- Instâncias são rodadas uma por vez. Variantes são embaralhadas de maneira determinística para mitigar viés de ordem; a semente não representa um controle de aleatoriedade interno do B&B.
- Threads físicas: Linux `lscpu` cruzado com afinidade, macOS `sysctl`, com fallback declarado. Auto usa max(1, cores − 1); valores manuais são aceitos.
- Provas relaxadas paralelas: variar `relax-proof-threads` mantendo `threads` do fallback inalterado. Close-stack paralelo: variar `closure-threads` mantendo threads do customer fallback inalteradas.
- `branching=auto` equivale atualmente a `close-stack`, portanto não se avalia como nova técnica independente.
- Para efeitos de microestruturas, `hash/sort/group` podem ser desligados no conjunto extended, mas isso pode prejudicar drasticamente a prova e aumentar o tempo; é um teste de ablação, não recomendação de operação.

## Informações coletadas

- Status, UB, LB e gap certificado após limite de tempo; tempos do solver e de parede; nós; counters de bound residual, tentativas/provas relaxadas, chamadas paralelas, fechamentos/fallbacks e prova de otimalidade.
- `reports/report.md`: comparação por dimensão e fator com diferença positiva padronizada como melhoria e tempo de prova comparável apenas quando ambos os braços atingem OPTIMAL.
- `reports/runs.csv`, `reports/by_configuration.csv`, `reports/comparisons.csv`: arquivos para análise em planilha e scripts.
- `manifest.json`: hashes SHA-256 dos executáveis e instâncias, argumentos e assinatura para retomada segura.

## Limitações

Uma instância por dimensão não permite inferência estatística para o grupo completo; recomenda-se aumentar para diversas instâncias após identificar variantes promissoras. Uma repetição mede um resultado específico da configuração, particularmente sujeito à variação de escalonamento em paralelo. Todos os TIME_LIMIT impedem qualquer afirmação de speedup na prova de otimalidade. Sempre consultar os perfis de fases para compreender a ativação real de relaxação e close-stack.

### Execução simplificada

```bash
TIME_LIMIT=3600 REPEATS=1 SUITE=core ./scripts/run_controlled_1h.sh results/controlled_core_1h
# após interrupção: mesmos parâmetros, sem misturar binários/instâncias
TIME_LIMIT=3600 REPEATS=1 SUITE=core RESUME=1 ./scripts/run_controlled_1h.sh results/controlled_core_1h
# estudo abrangente após concluir o núcleo
TIME_LIMIT=3600 REPEATS=3 SUITE=extended ./scripts/run_controlled_1h.sh results/controlled_extended_1h
```

A campanha `core` executa 12 configurações por dimensão, 48 execuções por repetição. Com limite de 3600 s, sua duração máxima nominal de B&B é de 48 horas por repetição, além de MCNh e overhead. A campanha `extended` executa 18 configurações por dimensão, 72 execuções por repetição (até 72 horas nominais). Como cada instância pode concluir antes do limite, a duração efetiva pode ser menor. `--comparisons` permite reduzir o escopo. O script não promete prosseguir se o computador for desligado; ele é retomável com `--resume`.

### Piloto de infraestrutura incluído no pacote

`results/controlled_full_smoke/` contém 72 execuções com 0,20 segundo por B&B, 16 comparações e 4 dimensões. Todos os arquivos foram lidos e nenhuma execução foi rejeitada pelo validador de certificados. O relatório desse piloto ilustra o formato final, mas seus resultados **não sustentam conclusões de velocidade**.
