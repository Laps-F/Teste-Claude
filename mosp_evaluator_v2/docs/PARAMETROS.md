# Dados necessários e contrato para o PT

## Dados fornecidos uma vez por instância

| Dado | Necessidade | Forma de fornecimento |
|---|---|---|
| Relação padrão–item | Obrigatória | `from_patterns(m, patterns)`, com uma lista de IDs de itens para cada padrão, ou matriz densa |
| Número total de itens/pilhas m | Obrigatório na forma esparsa | Primeiro argumento de `from_patterns`; pode incluir itens sem incidência |
| Número de padrões n | Obrigatório como informação, derivado na API | `patterns.size()` ou dimensão da matriz correspondente aos padrões |
| Número de linhas e colunas | Obrigatório na forma densa | Argumentos `rows`, `columns`; vetor plano em ordem de linhas |
| Orientação da matriz | Obrigatória na forma densa | `PatternsInRows` ou `PatternsInColumns`; não existe autodetecção |
| Regra de presença | Obrigatória na forma densa | `EqualToOne` conta somente 1; `Positive` conta valores >0 |
| Base de indexação de origem | Somente no adaptador de entrada | Converter uma vez para IDs de 0 a n-1 e de 0 a m-1; a CLI oferece `--index-base` para a solução |

Na forma esparsa a orientação já é explícita: cada vetor externo é um
padrão, e seus elementos são os IDs das pilhas daquele padrão. Não é
necessário fornecer uma regra de presença adicional.

Uma instância com zero padrões e/ou zero itens é aceita e tem objetivo
zero. O número de padrões é mantido mesmo se alguns forem vazios ou
tenham conjuntos de itens idênticos; padrões distintos continuam tendo
IDs distintos. IDs de itens dentro de um padrão são deduplicados, pois
a relação usada pelo MOSP é binária.

## Dados fornecidos a cada avaliação

| Interface | Parâmetros necessários | Estado prévio |
|---|---|---|
| `Evaluator::evaluate(solution)` | Permutação completa dos padrões | Somente a instância; nenhuma solução de referência |
| `IncrementalEvaluator::evaluate(solution)` | Permutação completa candidata | Estado construído para uma solução de referência da mesma instância |
| `IncrementalEvaluator::evaluate(move)` | Tipo do movimento, origem e destino | Estado corrente; os padrões do intervalo são obtidos internamente |
| `IncrementalEvaluator::apply(solution/move)` | Mesma solução ou movimento que se deseja tornar corrente | Atualiza incondicionalmente o estado de referência |

A solução é um `std::vector<int>` contendo cada ID de padrão exatamente
uma vez. `Move` aceita `Insert`, `Swap` ou `Reverse`. As posições são
baseadas em zero. `Insert(from,to)` remove o padrão em `from` e o coloca
na posição final `to`, deslocando os intermediários. `Reverse` inverte
todo o intervalo entre os extremos, ambos inclusivos; admite extremos
em qualquer ordem. Uma origem igual ao destino é um movimento nulo
válido, desde que a posição exista. Em uma instância sem padrões não há
movimento de posição válido.

O retorno de toda avaliação é o objetivo candidato exato, e não o delta.
No modo incremental o chamador obtém o delta por
`candidate_energy - state.value()`. O sinal positivo representa piora
para a minimização. `apply` retorna o novo objetivo do estado.

## Informações que não precisam ser parâmetros

Não é necessário fornecer demandas/contagens de ocorrências, primeiras
e últimas posições, perfil de pilhas, valor corrente declarado,
prefixos/sufixos ou uma máscara de itens únicos. Todas essas informações
são derivadas das incidências e da permutação. Retirar esses argumentos
evita divergências entre dados redundantes.

Temperatura, número de réplicas, política de aceitação, gerador aleatório,
semente, probabilidades das vizinhanças e regra de troca de réplicas
pertencem ao PT. Eles não afetam o significado do objetivo MOSP e não
entram na biblioteca. Uma demanda física maior que um deve ser convertida
em presença, quando esse for o modelo pretendido pelo algoritmo hospedeiro.

Não se pede limite alvo k para encerrar a contagem: se o PT precisa
calcular a probabilidade de aceitar uma piora, um valor truncado pode
alterar essa probabilidade. Esta versão sempre retorna o valor completo.

## Escolha de interface para a integração

1. Se o PT tem somente um callback que recebe a solução completa, use
   `Evaluator::evaluate`. Esse é o acoplamento mínimo e tem estado de
   trabalho pequeno.
2. Se o PT mantém uma solução por réplica e pode comunicar a aceitação,
   use um `IncrementalEvaluator` por réplica. Avalie a candidata; ao aceitar,
   chame `apply`. Uma rejeição não exige nenhuma chamada de recuperação.
3. Se o gerador conhece o tipo e os extremos do movimento, prefira a
   sobrecarga com `Move`. Ela elimina a validação e varredura completas
   da solução, preservando a validação das posições e do tipo do movimento.
4. Se a candidata vem de recombinação ou de uma perturbação arbitrária,
   `evaluate(solution)` continua exato: o intervalo é detectado
   automaticamente. Se esse intervalo cobre quase toda a sequência,
   o ganho incremental depende da densidade e do número de itens.

O PT pode usar `state.solution()` como configuração corrente. Se mantiver
uma cópia própria da permutação, deve atualizá-la ao aceitar um movimento.
Nas trocas de réplicas, transfira juntos configuração, objetivo, perfil
e prefixos/sufixos, trocando os objetos completos, ou reconstrua o estado
a partir da nova configuração. A temperatura pertence à posição da réplica.

Uma referência incremental antiga ainda permite avaliar qualquer
permutação completa corretamente, porém pode aumentar o intervalo de
diferenças. Um `Move` é sempre interpretado sobre `state.solution()`;
por isso sua sincronização com a configuração do PT é obrigatória.

## Políticas e métricas da versão 2

A seleção de cálculo é `Backend::Full`, `Backend::Incremental` ou
`Backend::Auto`. O modo automático usa um `SelectionPlan` imutável,
compartilhável entre réplicas da mesma instância. Se nenhum plano for
fornecido ao construtor adaptativo, ele é calibrado naquele momento;
para várias réplicas, prefira calibrar uma vez e compartilhar o resultado.

A função de energia é escolhida separadamente: `Energy::MOSP` ou
`Energy::MMOSP`. Os resultados de `evaluate_metrics` e do avaliador
adaptativo incluem o máximo inteiro, a soma do perfil em 64 bits e o
número de padrões. `metrics.energy(mode)` calcula o escalar em double.
Nenhuma dessas escolhas altera a interpretação da incidência ou a
convenção inclusiva de pilhas abertas.

`CalibrationOptions` oferece número de amostras por grupo, repetições
e semente de seu gerador privado. Os padrões são seis amostras, três
repetições e semente 20261029. Tempos e decisões podem ser inspecionados
ou restaurados com `SelectionPlan::from_decisions`. Entre comprimentos
com tempos positivos, o seletor interpola os custos; tabelas de tempos
zero usam decisões explícitas por bucket.

O repositório PT-MOSP foi disponibilizado e sua representação foi
verificada: `SEQUENCE=0` contém padrões; `SEQUENCE=1` contém peças e
requer decodificação. O adaptador e seus parâmetros estão documentados
em `../adapters/pt_mosp/README.md`; o guia geral está em `GUIA_PT_BR.md`.
A interpretação dos movimentos por peça é feita antes de identificar
qual intervalo de padrões mudou.

Esta API representa o MOSP de uma permutação completa de padrões,
com pilhas não ponderadas e extremos inclusivos. Ordens por peças
podem ser usadas com o decodificador opcional. Sequências com omissões,
repetições ou pesos por pilha exigem outro contrato de avaliação.
