# Plano técnico — Reintegração completa da relaxação/fusão de Chu–Stuckey (2009)

Data: 28/10/2026. Escopo: planejamento (sem alteração do código-fonte nesta etapa).

## Objetivo

Integrar a estratégia original de fusão/desfusão com **prova exata na instância relaxada** ao B&B moderno, preservando os resultados ótimos, soluções viáveis e certificados de otimalidade, o limite de tempo, o paralelismo e os três modos de branching. Suportar instâncias de até 1000x1000; não antecipar ganhos sem benchmark.

## Diagnóstico do código disponível

- O código-fonte atualmente disponível no pacote `csbb_modern_batch_presets_v1.tar.gz` implementa `--relaxation off|root-merge-probe`.
- `relaxation_probe(k)` faz fusões sobre cópias de `cross_`, constrói uma solução candidata heurística, a expande e a valida para a instância original; não resolve exatamente a instância relaxada para provar inviabilidade.
- `decision_search(k)` faz probe, eventualmente `decision_search_close_stack`, depois usa `decision_search_customer` como mecanismo de certificação.
- Não estão acessíveis neste ambiente os arquivos originais `mosp.c` e `solver.h` enviados anteriormente; serão necessários para auditoria linha a linha, assim como o artigo de 2009 original, antes da implementação definitiva.

## Regra lógica de correção

Se I' é relaxação válida de I por fusão, então OPT(I') <= OPT(I). Para um upper bound U comprovadamente viável em I: se o solver provar que I' não possui solução com valor <= U-1, conclui-se OPT(I') >= U e, portanto, OPT(I) = U. Se I' for viável com custo <= U-1, a ausência de prova demanda um unmerge e nova tentativa (ou fallback exato em I). Qualquer interrupção deve retornar `UNKNOWN`, nunca `INFEASIBLE`.

A validade da relação OPT(I') <= OPT(I) para a representação efetivamente implementada não pode ser presumida de uma alteração de bitsets: precisa ser demonstrada e testada.

## Etapas de implementação

### 0. Congelamento e auditoria do legado

1. Reobter `mosp.c`, `solver.h` e o artigo de Chu & Stuckey (2009).
2. Identificar precisamente as rotinas de escolha de pares, `relax`, `mergenodes`, `unmergenodes`, `findunmerge`, reconstrução/extensão de soluções, limites e flags.
3. Criar baseline reprodutível para o legado e para o moderno, com checksum dos arquivos, compilador, opções e resultados.

Aceite: mapeamento algoritmo-função-estados e testes reproduzíveis; nenhuma mudança no solver ainda.

### 1. Modelo formal e invariantes da fusão

1. Fixar o mapeamento entre cliente original e nó relaxado, adjacências, representantes ativos e membros do grupo.
2. Definir os efeitos exatos da fusão na matriz/grafo; auditar simetria das adjacências, autovínculos e cardinalidades.
3. Formalizar propriedade de relaxação e reversibilidade da transformação.
4. Comparar com um oráculo exato em instâncias pequenas aleatórias para verificar, em cada prefixo da sequência de fusões, `OPT(relaxada) <= OPT(original)`.

Aceite: prova/invariante documentado e testes de propriedade sem contraexemplos; falha bloqueia ativação de `full`.

### 2. Implementação da representação de relaxação e undo

1. Criar estruturas próprias para grupos, adjacências dinâmicas e histórico de merges (`MergeRecord`).
2. Implementar `merge`, `unmerge`, `rollback_to(depth)` com log incremental de alterações; evitar cópia completa do grafo por tentativa.
3. Permitir configurar estratégia de fusão, inicialmente a fórmula F(c) e critério de vizinho do artigo; preservar opção alternativa somente após baseline.
4. Verificar comportamento com bitsets dinâmicos até 1000 elementos, limites de memória e sanitizers.

Aceite: aplicar/desfazer qualquer sequência de fusões restaura byte-a-byte o estado lógico do grafo e os mapeamentos.

### 3. Prova exata da instância relaxada (primeiro serial)

1. Adaptar o motor de decisão customer search para resolver I' sob alvo k=U-1 sem compartilhar nogoods inválidos entre diferentes grafos.
2. Separar estados `FEASIBLE`, `INFEASIBLE_PROVED`, `UNKNOWN_TIMEOUT`, `UNKNOWN_NODE_LIMIT`, `UNKNOWN_RESOURCE`.
3. Revalidar a viabilidade de soluções de I' e usar a exatidão da prova relaxada apenas quando os invariantes forem satisfeitos.
4. Impedir exportação de certificado se o motor é interrompido ou se um subproblema ficou sem explorar.

Aceite: prova da relaxada produz o mesmo resultado que um solver/oráculo independente para testes pequenos.

### 4. Loop completo de merge/unmerge e aproveitamento de soluções

1. Partir de um upper bound U viável e validado na instância original.
2. Executar fusões até limiar planejado, observando política de fusão do legado.
3. Empregar heurística rápida na relaxada para detectar rapidamente casos em que `relaxed_ub < U`.
4. Se não houver solução barata, fazer tentativa de prova exata da relaxada com target U-1.
5. Se houver viabilidade abaixo de U, desfazer fusões e tentar estender/levantar o candidato, conforme estratégia original.
6. Se a relaxada for comprovadamente inviável sob U-1, atualizar lower bound certificado do original para U e terminar.
7. Se não houver prova antes de o grafo original ser restabelecido, recorrer à busca customer exata na instância original.

Aceite: nenhuma atualização de LB sem certificado exato; ausência de prova não cria falsa otimalidade.

### 5. Integração controlada com modos, bounds, paralelismo e limite de tempo

1. Acrescentar `--relaxation full`, mantendo `off` e `root-merge-probe` inalterados.
2. Ativar inicialmente apenas com `--branching customer --threads 1`; depois testar close-stack/auto com certificação customer e execução paralela.
3. Limitar tempo compartilhado da campanha: heuristic + merge + prova + unmerge + fallback dentro do limite total (sem resetar relógio).
4. Gerir a memória do subsolver e separar tabelas de dominância entre instâncias relaxadas distintos, exceto se houver prova de transferência válida.
5. Enquanto a auditoria geral de warm starts não estiver concluída (passo 2 do plano global), exigir incumbent original com sequência verificada para o modo `full` ou construir um incumbent próprio verificado; rejeitar input só com valor declarado.
6. Preservar compatibilidade do CSV/JSON, acrescentando campos sem eliminar nomes antigos.

Aceite: modos `off` e `root-merge-probe` reproduzem a baseline; `full` só melhora prova e nunca enfraquece corretude.

### 6. Regressão formal e testes adversariais

1. Executar a implementação original, o moderno sem relaxação, o root-probe e o full na mesma bateria pequena/média.
2. Rodar testes de propriedade com fusões sequenciais, graus extremos, componentes desconexas, nós gêmeos e padrões duplicados.
3. Rodar timeout durante merge, prova e unmerge; verificar que todos retornam UB/LB/gap sem certificado falso.
4. Testar seed/warm start válido e inválido; testar limite de nós, memória, variações de thread.
5. Validar o valor ótimo e uma sequência viável, não exigir mesma sequência entre algoritmos devido a empates.
6. Aplicar ASan/UBSan e testes determinísticos de regressão; preservar logs e metadados.

Aceite: equivalência de ótimo nas instâncias resolvidas, nenhuma violação de invariantes nem falsas provas.

### 7. Benchmark e calibração para instâncias até 1000x1000

1. Usar amostras reais 400x400, 600x600, 800x800, 1000x1000 e instâncias pequenas/médias.
2. Comparar mesmas CPUs, versões de compilador, uso de threads e condições de warm start, com budgets iguais.
3. Medir tempo de prova, melhor UB, LB, gap no timeout, nós explorados, tamanho das relaxadas, merges/unmerges, custo de cada fase, memória de pico e quantidade de provas efetuadas.
4. Não antecipar ganhos de ordens de magnitude: decidir ativação padrão apenas com resultados.

Aceite: tabelas e logs reproduzíveis, relatório de desempenho e decisão documentada de preset.

## Interface mínima proposta

- `--relaxation off|root-merge-probe|full`
- `--relax-merge-policy original` (padrão do full; outras políticas apenas após estudo)
- `--relax-max-merges N` (limite opcional)
- `--relax-max-time-share P` (limite opcional, fração do orçamento total; excesso exige fallback/UNKNOWN)
- `--profile 1` mantém formato existente e acrescenta métricas da relaxação.

Não é necessário expor opções de baixo nível de undo/hash ao usuário neste momento.

## Saída e indicadores adicionais

`relaxation_mode`, `relaxed_proof_status`, `relaxed_proofs_attempted`, `relaxed_proofs_completed`, `relaxed_proof_infeasibilities`, `relaxation_merge_count`, `relaxation_unmerge_count`, `relaxation_customers_at_proof`, `relaxation_time_seconds`, `relaxed_proof_time_seconds`, `original_fallback_used`, `proof_source`.

Manter sempre `status`, `upper_bound`, `lower_bound`, `gap_percent`, `time_seconds`/`elapsed_seconds`, `nodes`, etc.; status OPTIMAL apenas com UB viável e LB certificado coincidentes.

## Compatibilidade / estratégia de release

- `--relaxation off` permanece o padrão por segurança até validação.
- `original-modernized`, `low-risk`, `close-stack` preservam seus presets existentes; criar variante adicional para `full` após campanha, sem alterar os três existentes.
- Preservar equivalência de valor ótimo e certificado; sequências ótimas podem divergir.
- O desenvolvimento de `full` deve ser separado do passo global 2 (auditoria geral de warm starts) e do passo 3 (lower bounds por estado), com uma salvaguarda local para incumbents não verificados.

## Dependências e bloqueios

1. Necessário reenvio dos fontes originais `mosp.c` e `solver.h` para engenharia fiel e comparação linha a linha; esses arquivos não estão acessíveis atualmente.
2. A implementação moderna está acessível; o plano foi fundamentado nela e no algoritmo publicado de 2009 (obtido por indexação web do PDF do autor).
3. Nenhum ganho quantitativo é afirmado antes da execução do benchmark.
