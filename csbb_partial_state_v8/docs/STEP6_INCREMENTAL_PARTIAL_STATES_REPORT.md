# Etapa 6 — Avaliação incremental dos estados parciais do B&B (v8)

## Motivação e escopo

O avaliador incremental de permutações completas foi integrado anteriormente à heurística de
refinamento do incumbent. Essa biblioteca não pode avaliar diretamente estados do customer
search. Esta etapa desenvolve um mecanismo específico da representação de Chu–Stuckey:

- O conjunto `opened` é monótono ao longo do caminho de busca.
- Para cada cliente remanescente `c`, a quantidade de vizinhos ainda não abertos é
  `u[c] = |cross[c] \ opened|`.
- Ao abrir o conjunto novo `D`, atualizamos `u[c] <- u[c] - |cross[c] ∩ D|`.
- Depois da visita ao filho, o mesmo delta é revertido, inclusive para clientes que
  foram fechados automaticamente dentro da chamada recursiva.

A atualização é implementada por visita às adjacências de cada cliente recém-aberto.
Uma cópia imutável da máscara dos remanescentes elegíveis no filho garante que o undo
seja completo mesmo quando `force_close` modifica o bitset `remaining` recursivamente.

## Locais acelerados

1. `force_close`: identifica clientes fecháveis pelo contador `u[c]==0`.
2. `generate_moves`: consulta `u[c]` antes de alocar o bitset de abertura; monta bitsets
   completos apenas para movimentos que passam no limite do custo imediato.
3. `residual_bound_prunes` (`next`, `core`): usa `u[c]` para avaliar exatamente o
   limitante da próxima decisão, sem recomputar diferenças de conjuntos por candidato.
4. `dfs_core`: aplica o delta na descida e a reversão no retorno, com caches privados
   por subárvore/worker. `dfs` inicia o cache uma vez por tarefa independente.

As regras de poda e certificados não foram alterados. A geração de fronteira continua
utilizando o caminho legado, e cada worker reconstrói o cache na raiz de sua tarefa.
Provas relaxadas utilizam o grafo contraído já instalado no contexto e igualmente
reconstroem o cache por subárvore. `close-stack` tem busca própria; a otimização
aplica-se ao fallback customer, sem modificar o branching experimental.

## Configuração

```bash
--partial-state-eval off|counts    # default off
--partial-state-verify 0|1       # default 0; requer counts; custo alto
```

No modo `counts`, `--partial-state-verify 1` recalcula `u[c]` no início de cada
estado recursivo e interrompe em caso de divergência. Esse modo é para regressão,
não para medição de desempenho. `off` mantém a execução padrão da v7.

Campos de saída: `partial_cache_builds`, `partial_neighbor_updates`,
`partial_candidate_checks`, `partial_force_checks`, `profile_partial_build_seconds`,
`profile_partial_updates_seconds` e `partial_state_eval`. A semântica de `nodes`
e demais métricas de certificação é preservada.

## Validação

- 76/76 regressões contra a implementação original de Chu–Stuckey.
- 314 execuções da bateria `tests/test_partial_state_eval.py`: equivalência
  dos valores ótimos e dos nós nas buscas seriais determinísticas; também
  são exercitadas buscas paralelas, relaxação completa, fallback close-stack,
  desativação de hash, ordenação e agrupamento, além de interrupções.
- 151/151 verificações do warm start MCNh.
- 152/152 verificações do avaliador standalone.
- 516 execuções da bateria de lower bounds residuais.
- 232 execuções adicionais de regressão das provas relaxadas paralelas.
- 31 verificações específicas de refinamento do incumbente.
- 8 execuções adicionais do piloto `partial_state_relax` nas quatro dimensões,
  todas com saída válida e parada por tempo.
- Verificação UBSan com relaxação completa e 2 threads em A_AP-9: ótimo 5.

A suíte ampla `scripts/test.sh` foi iniciada, mas a execução do teste legado
`test_contractions` ultrapassou o limite desta sessão e não foi finalizada.
O script dedicado `scripts/test_partial_state.sh` foi executado integralmente e
passou. A regressão dos módulos anteriores também foi executada separadamente.


## Piloto de desempenho — resultados exploratórios

Campanha controlada de **0,5 segundo por execução**, com uma instância real de
cada dimensão e apenas uma repetição. O indicador é milhares de nós totais por
segundo; as execuções paralelas incluem nós da fronteira. Nenhum resultado
atingiu `OPTIMAL`, portanto a tabela NÃO demonstra redução do tempo de prova.

| Dimensão | Configuração | Legado (mil nós/s) | Cache (mil nós/s) | Razão | UB final |
|---|---|---:|---:|---:|---|
| 400×400 | serial | 6.46 | 11.32 | 1.75× | 92 → 83 |
| 600×600 | serial | 4.25 | 4.89 | 1.15× | 141 → 140 |
| 800×800 | serial | 2.64 | 3.85 | 1.45× | 184 → 182 |
| 1000×1000 | serial | 1.79 | 2.78 | 1.55× | 249 → 248 |
| 400×400 | paralelo (2 threads) | 24.37 | 24.63 | 1.01× | 91 → 91 |
| 600×600 | paralelo (2 threads) | 11.60 | 16.22 | 1.40× | 142 → 140 |
| 800×800 | paralelo (2 threads) | 6.57 | 7.12 | 1.08× | 184 → 184 |
| 1000×1000 | paralelo (2 threads) | 4.38 | 2.94 | 0.67× | 249 → 250 |

O custo de atualizar o cache é proporcional à soma dos graus dos clientes
recém-abertos, podendo exceder o custo de recomputar as operações bitwise
quando o grafo é denso. Não há ainda evidência de ganho consistente no modo
paralelo, nem em execuções longas. O modo `off` permanece padrão.

## Próximo estágio experimental

- Comparar ambos os modos com o mesmo warm start certificado, três ou mais
  repetições e limites de tempo mais longos.
- Analisar separadamente throughput customer, CPU time, pico de RAM, tempo
  até melhorar UB, provas relaxadas e tempo até OPTIMAL quando disponível.
- Investigar uma política adaptativa baseada no grau/densidade apenas após
  a análise com os dados de profiling.
- Para o modo de fronteira, avaliar propagação de contadores sem reconstrução
  por tarefa quando o custo de inicialização se mostrar relevante.
