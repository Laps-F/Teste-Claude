# Relatório técnico — Provas exatas paralelas sobre instâncias relaxadas

Data: 28 de outubro de 2026. Base: `csbb_residual_bounds_v3`. Nova versão: `csbb_parallel_relaxed_proofs_v4`.

## Objetivo e implementação

Antes desta atualização, `solve_contracted_exact()` trocava temporariamente a matriz de adjacência pela representação contraída e executava `dfs()` serialmente. Agora a função aceita uma execução paralela do mesmo problema de decisão, em uma fronteira limitada. As fusões, desfusões e seleção de contrações continuam sequenciais, e nenhuma modificação do grafo é realizada enquanto seus trabalhadores estiverem ativos.

- Cada tentativa de prova em grafo contraído cria sua própria fronteira e um número de trabalhadores `min(threads_solicitadas, folhas_da_fronteira)`.
- Cada trabalhador recebe sua própria tabela de dominância e seu próprio contexto DFS; não são compartilhados registros de estados de versões diferentes do grafo.
- O contador de nós é global e atômico, o deadline global e o deadline de relaxação são respeitados por todos os trabalhadores, e exceções dos trabalhadores são propagadas após as threads serem reunidas.
- O estado `INFEASIBLE` só é retornado quando **todas** as subárvores da fronteira foram pesquisadas sem interrupção e sem solução. Resultados `TIME_LIMIT`, `NODE_LIMIT`, `RelaxBudget` e falhas de alocação não constituem prova.
- Uma solução viável encontrada no grafo relaxado é validada após lift para a instância original antes de ser usada no B&B.
- O limite `--relax-proof-frontier-cap` evita multiplicação irrestrita de folhas: quando uma divisão excederia o máximo, a subárvore inteira é preservada como folha. Isso garante completude do particionamento e mantém o custo de armazenamento controlado.

## Interface

- `--relaxation full`: ativa provas exatas em grafos contraídos.
- `--relax-proof-threads N`: 0 (padrão) herda `--threads`; 1 obriga prova serial.
- `--relax-proof-depth D`: -1 (padrão) herda `--parallel-depth`; 0 obriga prova serial.
- `--relax-proof-frontier-cap N`: número máximo de folhas da fronteira, padrão 8192; valor positivo obrigatório.
- `--relax-max-time-share P`: compartilha o limite global de tempo com o B&B original.

Saídas JSON/texto acrescentadas: `relaxed_proof_parallel_calls`, `relaxed_proof_frontier_states`, `relaxed_proof_max_workers`, `relax_proof_threads`, `relax_proof_depth`, `relax_proof_frontier_cap`.

O modo `--relaxation off` não executa provas relaxadas. `--branching close-stack` permanece com a mesma estratégia de prova/fallback; a paralelização desta etapa é exclusiva do resolvedor de instâncias relaxadas.

## Validação

1. **76/76** casos de regressão do código anterior, confrontados com a implementação de Chu–Stuckey de 2009 e com um oráculo exato, sem divergências.
2. **58 instâncias × 4 variantes = 232 execuções** na regressão específica de provas contraídas seriais/paralelas, incluindo três opções de trabalhadores/limitantes residuais, sem divergências do ótimo.
3. Foram efetivamente lançadas **102 provas paralelas** durante os testes de 58 instâncias.
4. **151** verificações de certificados MCNh aprovadas.
5. **516** execuções de regressão dos lower bounds residuais aprovadas.
6. Limites de nós, prazo de relaxação, herança automática, flags inválidas e capacidade de fronteira reduzida foram testados. Um teste com AddressSanitizer/UndefinedBehaviorSanitizer concluiu no exemplo A_AP-9, com ótimo 5 e 3 chamadas paralelas de prova sem erros reportados.
7. Automação em lote executada sobre duas instâncias reais 400×400 com parada por tempo, mantendo saída JSON, CSV consolidado e logs.

## Piloto exploratório — instâncias reais

Limite de **1,5 segundo por execução**, sem MCNh e com 1 ou 4 trabalhadores para as provas relaxadas; amostra insuficiente para estabelecer vantagem estatística.

| Dimensão | Trabalhadores | UB | LB | Gap reportado | Provas paralelas |
|---|---:|---:|---:|---:|---:|
| 400×400 | 1 | 80 | 7 | 1042.857143% | 0 |
| 400×400 | 4 | 82 | 7 | 1071.428571% | 6 |
| 600×600 | 1 | 140 | 9 | 1455.555556% | 0 |
| 600×600 | 4 | 141 | 9 | 1466.666667% | 1 |
| 800×800 | 1 | 184 | 9 | 1944.444444% | 0 |
| 800×800 | 4 | 183 | 9 | 1933.333333% | 1 |
| 1000×1000 | 1 | 257 | 11 | 2236.363636% | 0 |
| 1000×1000 | 4 | 259 | 11 | 2254.545455% | 1 |

Todas as oito execuções terminaram por `TIME_LIMIT`. A solução obtida depende da ordem de exploração e do orçamento temporal e **não se pode inferir ganho de velocidade** deste piloto. Os resultados mostram que a versão paralela alcançou estados internos com trabalhadores efetivos, e que nenhum estado de término prematuro foi tratado como certificado de otimalidade.

## Recomendação operacional

Para iniciar uma campanha controlada em 400×400, utilizar `--branching customer --relaxation full --threads N --parallel-depth 1 --relax-proof-threads 0 --relax-proof-depth 1 --relax-proof-frontier-cap 8192 --time-limit 3600`. Comparar com a mesma configuração usando `--relax-proof-threads 1`; medir tempo de prova de otimalidade, nós, número de provas relaxadas, tempo na relaxação, uso de CPU e memória em repetições/instâncias.

**Limites desta entrega:** não há ainda evidência de speedup significativo nas instâncias grandes; a etapa de paralelização de `close-stack` permanece separada. A infraestrutura do resolvedor original continua disponível como fallback exato.
