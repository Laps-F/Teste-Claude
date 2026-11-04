# Relatório automático de comparação controlada — MOSP / Chu–Stuckey

Gerado (UTC): 2026-10-30T16:44:36+00:00.
**Campanha:** 8 execuções registradas; 0 ainda não realizadas; 0 inválidas/falhas.
**Protocolo:** uma instância real por dimensão, quatro dimensões (400, 600, 800, 1000), 1 repetição(ões), 0.5 s de B&B por execução, 2 trabalhadores para modos paralelos.
**Controles:** cada par modifica somente o fator descrito na matriz experimental; mesmos arquivos, binário, limite de tempo, e mesmo warm start certificado para todas as condições que exigem MCNh. Execuções são SEQUENCIAIS. Custos de pré-cálculo do MCNh são contabilizados separadamente.

## Interpretação obrigatória

- `ΔUB = UB(controle) − UB(teste)` (positivo favorece teste); `ΔLB = LB(teste) − LB(controle)` (positivo favorece teste); `Δgap = gap(controle) − gap(teste)` (positivo favorece teste).
- Speedup de **prova de otimalidade** = tempo do controle ÷ tempo do teste, calculado **somente quando ambas as variantes certificaram OPTIMAL em todas as repetições válidas**. TIME_LIMIT não prova speedup.
- A amostra contém apenas uma instância por dimensão e, com uma repetição, não permite inferência estatística sobre a família de instâncias. Nenhum resultado abaixo representa desempenho comprovado para 610 instâncias.
- Throughput é nós totais/segundo de parede, inclui as fases customer, relaxada, closure e fronteira; comparação por fase está no `runs.csv`. Não equivale a velocidade de prova.
- Work inflation somente é calculado se ambos concluem OPTIMAL e a ablação mantém a estrutura de busca; contagens em TIME_LIMIT não são equivalentes a trabalho total.
- AUC-gap usa interpolação trapezoidal dos registros periódicos, normalizada pelo último instante; menor é melhor. Cadência e duração afetam comparabilidade.
- Utilização de worker é estimada como tempo em tarefas / tempo de janela de despacho; tempos inativos incluem inicialização/sincronização e não são medidas instrumentadas de bloqueio.
- Os slots de worker são reutilizados em fases sucessivas e não identificam threads físicas persistentes.
- A busca `close-stack` pode recorrer ao `customer` após uma falha; seu resultado mede o custo total da política com fallback, não uma prova exclusiva do método close-stack.
- `full` possui fase própria de relaxação e fallback; os perfis de fases devem ser analisados conjuntamente. A confirmação de um lower bound certificado permanece responsabilidade do solver.

## Situação dos experimentos por dimensão

| Dimensão | Execuções válidas / previstas | OPTIMAL | TIME_LIMIT | Erros |
|---:|---:|---:|---:|---:|
| 400×400 | 2/2 | 0 | 2 | 0 |
| 600×600 | 2/2 | 0 | 2 | 0 |
| 800×800 | 2/2 | 0 | 2 | 0 |
| 1000×1000 | 2/2 | 0 | 2 | 0 |

## Comparação pareada por melhoria

### Cache incremental em prova relaxada (`partial_state_relax`)

Controle: `partial_state_relax_off`; teste: `partial_state_relax_counts`. Apenas --partial-state-eval off→counts, relaxação full igual.

| Dim. | Status C/T | UB C→T | LB C→T | Gap C→T (%) | ΔUB | ΔLB | Δgap (p.p.) | Speedup prova | Interpretação |
|---:|---|---|---|---|---:|---:|---:|---:|---|
| 400 | TIME_LIMIT/TIME_LIMIT | 92.00→91.00 | 7.00→7.00 | 1,214.29→1,200.00 | 1.00 | 0.00 | 14.29 | — | MELHOR UB; SEM PROVA DE SPEEDUP |
| 600 | TIME_LIMIT/TIME_LIMIT | 142.00→141.00 | 9.00→9.00 | 1,477.78→1,466.67 | 1.00 | 0.00 | 11.11 | — | MELHOR UB; SEM PROVA DE SPEEDUP |
| 800 | TIME_LIMIT/TIME_LIMIT | 184.00→184.00 | 9.00→9.00 | 1,944.44→1,944.44 | 0.00 | 0.00 | 0.00 | — | EMPATE NOS BOUNDS; SPEEDUP NÃO DETERMINADO |
| 1000 | TIME_LIMIT/TIME_LIMIT | 249.00→249.00 | 11.00→11.00 | 2,163.64→2,163.64 | 0.00 | 0.00 | 0.00 | — | EMPATE NOS BOUNDS; SPEEDUP NÃO DETERMINADO |

## Eficiência computacional e paralelismo

Throughput em milhares de nós/s. Aceleração de throughput é apresentada mesmo sob TIME_LIMIT, mas NÃO demonstra speedup de prova. Work inflation somente em provas pareadas completas comparáveis.

| Dim. | Comparação | Nós/s C→T | Ganho throughput | Work inflation | Eficiência paralela | ΔAUC gap (p.p.) |
|---:|---|---|---:|---:|---:|---:|
| 400 | `partial_state_relax` | 16.39→17.32 | 1.06 | — | — | 3.74 |
| 600 | `partial_state_relax` | 7.57→9.73 | 1.29 | — | — | 2.93 |
| 800 | `partial_state_relax` | 5.94→6.34 | 1.07 | — | — | 1.48 |
| 1000 | `partial_state_relax` | 3.63→3.78 | 1.04 | — | — | -0.06 |

### Consumo de recursos e distribuição de trabalho

As taxas e utilizações são medianas por execução. `idle` inclui overhead estimado de despacho e não representa necessariamente CPU ociosa.

| Dim. | Configuração | Nós/s | CPU (s) | RSS pico (MB) | AUC gap (%) | 1ª melhora LB (s) | Utilização customer | Utilização relaxada | Utilização closure |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 400 | `partial_state_relax_counts` | 17,324 | 0.80 | 97.39 | 1,294.96 | — | 0.953 | 0.726 | — |
| 400 | `partial_state_relax_off` | 16,390 | 0.80 | 97.39 | 1,298.70 | — | 0.961 | 0.780 | — |
| 600 | `partial_state_relax_counts` | 9,735 | 0.82 | 97.39 | 1,499.64 | — | 0.971 | — | — |
| 600 | `partial_state_relax_off` | 7,565 | 0.84 | 97.39 | 1,502.57 | — | 0.985 | — | — |
| 800 | `partial_state_relax_counts` | 6,336 | 0.79 | 130.88 | 1,962.08 | — | 0.987 | — | — |
| 800 | `partial_state_relax_off` | 5,939 | 0.84 | 128.38 | 1,963.56 | — | 0.986 | — | — |
| 1000 | `partial_state_relax_counts` | 3,778 | 0.79 | 240.44 | 2,172.57 | — | 0.977 | — | — |
| 1000 | `partial_state_relax_off` | 3,626 | 0.82 | 234.76 | 2,172.51 | — | 0.985 | — | — |

**Séries temporais:** `reports/progress_samples.csv` reúne snapshots com UB/LB/gap, nós por fase e provas relaxadas concluídas, identificados por dimensão, variante e repetição.

## Diagnóstico das melhorias ativadas

Valores abaixo são medianas por execução. Zero pode indicar que o mecanismo foi habilitado mas não produziu atividade mensurável.

| Dim. | Configuração | LB prunes | Provas relaxadas paralelas | Close-stack paralelo | Fallback close-stack | Tempo full (s) | Tempo prova relaxada (s) |
|---:|---|---:|---:|---:|---:|---:|---:|
| 400 | `partial_state_relax_counts` | 0 | 1 | 0 | 0 | 0.131 | 0.002 |
| 400 | `partial_state_relax_off` | 0 | 1 | 0 | 0 | 0.129 | 0.002 |
| 600 | `partial_state_relax_counts` | 0 | 0 | 0 | 0 | 0.129 | 0.000 |
| 600 | `partial_state_relax_off` | 0 | 0 | 0 | 0 | 0.127 | 0.000 |
| 800 | `partial_state_relax_counts` | 0 | 0 | 0 | 0 | 0.129 | 0.000 |
| 800 | `partial_state_relax_off` | 0 | 0 | 0 | 0 | 0.129 | 0.000 |
| 1000 | `partial_state_relax_counts` | 0 | 0 | 0 | 0 | 0.131 | 0.000 |
| 1000 | `partial_state_relax_off` | 0 | 0 | 0 | 0 | 0.131 | 0.000 |

## Custo de preparação dos warm starts MCNh

Computado uma vez por instância e modo de preprocessing; não incluído no limite de tempo do B&B.

| Dimensão / MCNh preprocessing | Tempo (s) | SHA-256 do certificado |
|---|---:|---|
| (1000, 'on') | 0.6655 | `7a95fa4e9b769500` |
| (400, 'on') | 0.0538 | `a07db9ea4484a8a1` |
| (600, 'on') | 0.1552 | `81dce63e56cf935b` |
| (800, 'on') | 0.3674 | `9f44a038fccb91a8` |

## Rastreabilidade e replicação

Assinatura da campanha: `9887f4e7c7ccea0b475f62e09ce12edde0308dcdf7e812a696b798d32d868401`.

- `manifest.json`: hash SHA-256 do solver, MCNh, dos quatro arquivos e especificação de cada fator.
- `warm_preparation.json`: tempo/código/hash de cada MCNh; certificado reutilizado nas execuções comparáveis.
- `runs/*.json`: metadados, comando, código de retorno e resposta JSON íntegra do solver.
- `logs/*.out` e `logs/*.err`: saídas individuais para auditoria.
- `reports/runs.csv`: dados primários tabulados; `by_configuration.csv`: sumários; `comparisons.csv`: deltas por par.

## Limites da conclusão

Não são declarados ganhos algorítmicos sem observações pareadas e válidas. Se todas as execuções de um par terminarem por limite de tempo, avaliar UB/LB/gap e custos de profiling, sem classificar uma variante como mais rápida para *provar* otimalidade.
