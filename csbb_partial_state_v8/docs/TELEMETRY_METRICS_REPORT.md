# Instrumentação da busca — B&B MOSP v6

## Escopo

A presente atualização instrumenta o resolvedor `csbb_modern` e os scripts `controlled_experiments.py`, `controlled_report.py` e `run_batch.py` sem modificar deliberadamente a lógica de branching, dominância, fusão/desfusão, MCNh, admissibilidade dos limitantes ou certificação de otimalidade.

## Medições no resolvedor

Os campos JSON adicionais incluem:

- `nodes_customer`, `nodes_relaxed_proofs`, `nodes_close_stack` e `nodes_frontier`: visitas a nós por fase. O somatório dessas categorias equivale ao campo legado `nodes` após cada execução normal, incluindo execuções incompletas.
- `nodes_per_second`: `nodes / elapsed_seconds`, throughput **global**, sujeito à mistura de custo entre as fases.
- `cpu_seconds`: tempo de CPU total do processo (`std::clock`). Pode exceder o tempo de parede quando há paralelismo.
- `peak_rss_mb`: pico de memória residente obtido via `getrusage` (Linux em KB, macOS em bytes).
- `workers_customer`, `workers_relaxed`, `workers_closure`: arrays por **slot lógico** de trabalhador (não uma identidade persistente de thread) com `tasks`, `nodes`, `active_seconds`, `available_seconds` e `idle_or_overhead_seconds`.
- `--progress-interval N --progress-file PATH`: gera CSV temporal com `time_seconds,nodes_total,nodes_customer,nodes_relaxed_proofs,nodes_close_stack,nodes_frontier,upper_bound,lower_bound,gap_percent,relaxed_proofs_completed`.

O monitor temporal usa apenas contadores atômicos e valores UB/LB publicados pela thread coordenadora depois das transições certificadas. Não inspeciona a tabela de hash nem os estados do DFS. O registro é desativado por padrão no executável (`--progress-interval 0`) e ativado por padrão nos scripts de campanha a cada 30 s.

### Cuidado com utilização e paralelismo

`available_seconds` representa a janela de despacho de cada grupo de trabalhadores, não CPU-time medido. `idle_or_overhead_seconds` é uma **estimativa agregada** de tempo não gasto em tarefas, incluindo startup, despacho e finalização. Não representa uma medida isolada de bloqueio em mutex. Cada execução paralela usa slots `0..N-1`; os slots são acumulados por fase entre os vários problemas de decisão e podem ser reutilizados em novas threads.

### Comparação entre algoritmos e regimes de interrupção

O relatório distingue métricas:

1. **Speedup de prova:** `T_serial/T_paralelo` somente quando ambos certificados `OPTIMAL` completos estão disponíveis.
2. **Eficiência paralela nominal:** speedup/p, apenas para a ablação que modifica a contagem de threads do customer search; p é o número configurado, não uma garantia de núcleos ocupados.
3. **Work inflation:** nós totais da variante paralela / nós totais do controle, somente quando ambos provaram ótimo e a comparação mantém a estrutura de busca (customer_parallel, relaxed_proof_parallel, close_stack_parallel). Não inferir com `TIME_LIMIT`.
4. **Throughput gain:** razão entre `nodes_per_second` do teste e controle; válido como produtividade medida mesmo em `TIME_LIMIT`, mas não como speedup de prova. Pode aumentar enquanto LB e gap permanecem iguais.
5. **AUC-gap:** integral trapezoidal do gap percentual ao longo da execução, normalizada pelo tempo de último snapshot. Menor é melhor. Registrar intervalo suficiente para permitir comparação, com séries temporais pareadas.
6. **Memória, CPU e distribuição de tarefas:** devem ser analisadas juntamente com as curvas de progresso da prova.

Os registros periódicos estão em `progress/*.csv`, o arquivo agregado para gráficos em `reports/progress_samples.csv`, a tabela comparativa em `reports/comparisons.csv`, o resumo por variante em `reports/by_configuration.csv` e o relatório interpretativo em `reports/report.md`. O script em lote simples utiliza `progress/INSTANCIA.csv` e adiciona os indicadores ao CSV/JSONL consolidado.

### Limitações

- Séries temporais são amostradas; melhoras instantâneas entre amostras não são reconstruídas. O tempo para primeira melhora de LB tem resolução limitada pelo intervalo.
- Em execução interrompida, o LB é o último **certificado global** publicado, não o menor bound dos ramos ativos (que ainda não está monitorado como uma estrutura dinâmica independente).
- `workers_*` soma períodos de disponibilidade de slots sobre múltiplas fases. Não equivale a instrumentação de espera em mutex nem aos contadores de CPU individuais por thread.
- `std::clock` mede CPU time do processo, não necessariamente tempo consumido somente por workers (inclui inicialização e monitoramento); o RSS é pico do processo, não memória exclusiva de uma fase.
- O piloto curto valida instrumentação e formato de relatório, não avalia ganho de prova.
- As taxas de nós de representações diferentes (customer, closure, relaxada) não são intercambiáveis como custo de trabalho homogêneo.

## Exemplos

```bash
./scripts/build.sh
python3 scripts/controlled_experiments.py --suite core --time-limit 3600 --threads auto \
   --progress-interval 30 --output-dir results/controlled_core_1h
python3 scripts/controlled_experiments.py --report-only --output-dir results/controlled_core_1h
python3 scripts/run_batch.py --instances-dir instances/frinhani_carvalho_soma/400 \
   --solver ./csbb_modern --mcnh ./mcnh_modern --preset low-risk \
   --threads auto --time-limit 3600 --progress-interval 30 \
   --output-dir results/batch_400
```
