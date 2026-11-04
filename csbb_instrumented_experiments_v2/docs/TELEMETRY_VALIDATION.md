# Validação da atualização de instrumentação — 28/10/2026

## Compilação e regressão

- `scripts/build.sh`: compilação de `csbb_modern` e `mcnh_modern` aprovada.
- Regressão exata `tests/run_regression.py`: 76 casos, 0 falhas, mesmo ótimo nas configurações originais e modernizadas.
- `tests/test_telemetry_pipeline.py`: 5 testes de funcionalidade e consistência, abrangendo customer paralelo, close-stack paralelo, provas relaxadas paralelas, modo sem monitor e rejeição de parâmetros de progresso incompletos.
- `tests/test_controlled_experiments.py`: 6 testes de protocolo e controle estatístico aprovados.

## Pilotos nas instâncias reais

- `results/telemetry_validation`: 8 execuções (customer serial/paralelo, duas threads) nas dimensões 400×400, 600×600, 800×800 e 1000×1000, 0,5 s cada; relatório e CSV completos, sem falhas de execução/certificado.
- `results/telemetry_multi_phase`: 24 execuções das ablações relaxação `full`, prova relaxada serial/paralela e close-stack serial/paralelo, 0,35 s cada, cobrindo as mesmas quatro dimensões; 0 falhas. Registro de nós nas fases efetivamente ativadas.
- `run_batch.py`: execução com MCNh e solver em 400×400 com limite curto, CSV/JSONL/logs e progresso produzidos.
- Todos os pilotos de comparação controlada terminaram por `TIME_LIMIT`. Nenhum speedup de prova foi inferido a partir deles.

## Invariantes comprovados nos testes instrumentados

- `nodes = nodes_customer + nodes_relaxed_proofs + nodes_close_stack + nodes_frontier` no retorno normal das execuções testadas.
- A última amostra de progresso coincide com o UB, LB e número de nós retornados pelo solver.
- Sem monitor: o solver preserva `OPTIMAL` e os campos de solução antigos.
- Comparações com provas incompletas não geram work inflation nem speedup de certificação; há apenas estatísticas de produtividade, bounds e AUC do gap.

## Limites

Os testes pilotos têm durações deliberadamente curtas. Não demonstram superioridade de desempenho das variantes ou representatividade estatística para o benchmark completo. Os tempos de atividade e disponibilidade dos slots de trabalhador são medições aproximadas por tarefa/fase, com overhead de instrumentação. O piloto verifica a automação e as salvaguardas, não a calibração experimental definitiva.
