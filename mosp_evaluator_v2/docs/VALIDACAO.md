# Validação da versão 2.0.0 — 29/10/2026

## Resultado e escopo

Foram compilados e executados o núcleo C++17, o seletor automático,
a energia MMOSP, o decodificador por peças, a ferramenta standalone e
uma adaptação do código real fornecido em `PT-MOSP-main.zip`.
Os testes funcionais e com AddressSanitizer/UndefinedBehaviorSanitizer
passaram sem divergências ou diagnósticos desses sanitizers.

O anexo inclui fontes e resultados históricos do PT-MOSP, mas não
inclui suas instâncias MOSP experimentais. Os testes e o piloto desta
versão usam matrizes sintéticas. A integração foi executada; não foi
realizado um novo benchmark de qualidade ou tempo total do PT nas
instâncias dos artigos.

## Núcleos e estatísticas

O avaliador integral usa contagens restantes e marcação de itens
iniciados. O incremental usa bitsets de prefixos/sufixos, perfil e
árvore de máximos. O oráculo de teste calcula primeiras/últimas
ocorrências e soma seus intervalos inclusivos. Também confere S pela
soma das extensões dos intervalos dos itens.

| Grupo | Cenários | Resultado |
|---|---:|---|
| Todas as matrizes de 2 itens × 4 padrões, permutações e três movimentos entre todos os pares | 294.912 | Passou |
| Todas as matrizes de 3 itens × 3 padrões, permutações e três movimentos entre todos os pares | 82.944 | Passou |
| Movimentos aleatórios encadeados do núcleo | 40.000 | Passou |
| Núcleo concorrente, oito trabalhadores | 8.000 | Passou |
| Cadeias `full`/`incremental`/plano misto forçado | 14.400 | Passou |
| Avaliação concorrente com plano calibrado compartilhado | 2.400 | Passou |
| Total dos cenários principais | 442.656 | Sem divergências |

Os cenários exaustivos incluem movimentos nulos e ambas as direções.
Foram comparados permutação, perfil completo, máximo, soma em 64 bits,
avaliação pura e estado após aplicação/desfazimento. São 11.201.883
verificações seriais na suíte base e 264.366 na suíte adaptativa, além
das verificações dos trabalhadores concorrentes.

As matrizes aleatórias abrangem 0, 1, 63, 64, 65, 249, 250, 251,
1000 e 1031 itens, densidades incluindo zero e um e até 65 padrões.
Há testes de entradas inválidas, instâncias simultâneas, cópia e troca
de estados, ocorrência única, incidências duplicadas, padrões vazios,
itens ausentes e dimensões não quadradas.

A MMOSP foi conferida no exemplo publicado: M=4, S=13, n=4 produz
4,8125; M=4, S=12, n=4 produz 4,75. Também foram testados o caso de
termo adicional igual a um, o caso vazio, uma soma sintética superior
a 32 bits e a preservação da prioridade de M em todos os perfis curtos
enumerados. O teste com soma sintética verifica o tipo e a fórmula;
não representa uma instância gigantesca efetivamente alocada.

O decodificador é confrontado com uma implementação independente para
todas as 120 ordens de cinco itens. Inclui padrões vazios, item sem
incidência e um caso de 1031 itens. O seletor é testado com tabelas
explícitas que forçam ambos os caminhos, interpolação em comprimentos
intermediários, restauração do plano e rejeição de planos de outra instância.

## Aceitação, rejeição e réplicas

`examples/adaptive_pt.cpp` executa 12.000 propostas por combinação de
energia e backend, com quatro réplicas, totalizando 72.000 propostas.
Para cada energia e semente, os três backends produziram exatamente
as mesmas decisões e configurações durante toda a execução:

| Energia | Aceitas | Rejeitadas | Pioras da energia aceitas | Trocas | Hash da trajetória |
|---|---:|---:|---:|---:|---:|
| MOSP | 6.942 | 5.058 | 778 | 218 | 9268521355388061847 |
| MMOSP | 6.517 | 5.483 | 2.009 | 176 | 9195654474554505231 |

As contagens de pioras usam a energia escolhida: na MMOSP incluem
movimentos com M constante e S maior. Os hashes servem para comparar
backends nesta compilação; não são uma promessa de identidade entre
implementações de bibliotecas padrão de plataformas diferentes.

O exemplo legado `examples/pt_adapter.cpp` continua exercitando a API
inteira de MOSP em mais 12.000 propostas. Ele não é o adaptador do
repositório anexado, que está isolado em `adapters/pt_mosp/`.

## Alocações e CLI

Uma sonda de alocações exclusiva do teste executou 10.000 iterações
depois de preparar o maior intervalo. Cada iteração faz 12 chamadas
distribuídas entre avaliação integral, incremental, seletor misto,
aplicação/desfazimento e decodificação. Registrou zero novas alocações
nos caminhos exercitados após esse aquecimento. A inicialização e a
primeira expansão dos buffers continuam alocando memória.

A CLI passou em 22 casos, incluindo os três backends, MMOSP, conversão
por peças, orientação, presença, índices, perfil e entradas inválidas.

## PT-MOSP/PTAPI real

O instalador foi executado sobre uma cópia limpa dos fontes recebidos.
Foram verificados os backups dos 11 arquivos originais alterados e a
instalação de 18 arquivos, incluindo fontes novos. A repetição escreveu
zero arquivos. Uma versão de origem com alteração local foi recusada
antes de qualquer escrita. O Makefile original compila `mainMOSP`, e
o Makefile adicional compila `mainMOSP_eval`.

O teste de integração exercita todas as combinações de:

- Dois formatos de corpo da matriz, com cabeçalho padrão do PT.
- Duas representações da solução: padrões e peças.
- Três movimentos: troca, inversão e inserção.
- Duas energias: MOSP e MMOSP.
- Três estratégias: integral, incremental e automática.

São 72 configurações, com quatro réplicas, MCL=16, PTL=8 e quatro
trabalhadores. Mais quatro casos degenerados exercitam construção e
movimentos em instâncias de um padrão, padrões vazios, nenhum item e
nenhum padrão. Cada rodada soma 37.632 propostas auditadas. A suíte
passou em compilação otimizada e em compilação ASan/UBSan.

No modo de auditoria, cada proposta é confrontada com a avaliação
integral; cada aceitação/troca confere a correspondência entre energia
e estado. A solução final de cada execução é verificada por um oráculo
Python de primeiras/últimas ocorrências, inclusive o perfil decodificado.
O arquivo legado de resultados foi conferido para manter o MOSP inteiro
na terceira linha. Propostas idênticas podem ser respondidas por cache
e não incrementam necessariamente os contadores de execução de núcleos.

`test_precision.cpp` verifica em compilação e execução que o acesso à
energia retorna double e preserva 4,75 e 4,8125 na troca real de soluções.
`test_construction.cpp` confirma que os dois formatos de matriz produzem
a mesma incidência e a mesma solução construtiva MCNH, em ambas as
representações. O gerador original do PT-MOSP mantém `random_device`;
essas execuções auditam correção e não constituem comparação pareada
de qualidade sob sementes controladas.

Para reproduzir os testes específicos, depois da instalação:

```sh
PT_MOSP_REPO=/caminho/PT-MOSP-main
python3 adapters/pt_mosp/test_integration.py --repo "$PT_MOSP_REPO"
g++ -std=c++20 -O2 -I"$PT_MOSP_REPO/PTAPI-main/include" \
  adapters/pt_mosp/test_precision.cpp -pthread -o /tmp/mosp_pt_precision
/tmp/mosp_pt_precision
g++ -std=c++20 -O2 -I"$PT_MOSP_REPO/MOSP" \
  -I"$PT_MOSP_REPO/MOSP/mosp_evaluator/include" \
  adapters/pt_mosp/test_construction.cpp "$PT_MOSP_REPO/MOSP/MOSP.cpp" \
  src/evaluator.cpp src/adaptive.cpp -pthread -o /tmp/mosp_pt_construction
/tmp/mosp_pt_construction tests/fixtures/pt_rows.txt tests/fixtures/pt_columns.txt
```

## Instrumentação e limites

Núcleo: GCC 13.3.0, C++17, `-O3 -Wall -Wextra -Wpedantic`, sem warnings
na compilação verificada. PTAPI: C++20/GCC, com warnings remanescentes
no código MCNH legado. A compilação instrumentada usa `-O1 -g
-fno-omit-frame-pointer -fsanitize=address,undefined`.

Os testes instrumentados foram executados com:

```sh
ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
make sanitize -j2
```

A detecção de vazamentos foi desativada por uma limitação já observada
do ambiente ao enumerar threads. Não se afirma auditoria de vazamentos
ou verificação por ThreadSanitizer. Os testes concorrentes verificam
resultados com objetos de trabalho separados; não substituem uma
auditoria formal de toda a infraestrutura de execução do PTAPI.

## Piloto de custo do seletor

O piloto usa n=m=200 ou 1000, incidências Bernoulli com densidades-alvo
2%, 20% e 80%, três movimentos, distâncias 1, n/7 e n/2, e ambas as
interfaces de entrada. As densidades efetivas são aleatórias; o seletor
usa o número real de incidências. São 108 workloads, três backends e
três repetições sequenciais, totalizando 972 medições e 324 medianas.

São 300 propostas por configuração de tamanho 200 e 100 para tamanho
1000. Metade é aplicada de forma prescrita, independentemente da energia.
O cálculo de MMOSP e a aplicação estão cronometrados. As três estratégias
recebem a mesma trajetória e suas somas de M, S e energia, e seu estado
final, são conferidos. Matriz, propostas, decisões, estado inicial e
aquecimento ficam fora da medição por proposta.

O custo de materializar a candidata é incluído quando a interface
recebe um movimento e usa o backend integral. Na interface de solução,
a candidata já existe como entrada. A calibração ocorre uma vez por
instância; seu tempo aparece separadamente nas linhas `auto` e precisa
ser amortizado/contabilizado pelo algoritmo hospedeiro. Não se somam os
valores repetidos dessa coluna para obter um custo total.

Exemplos de medianas, em microssegundos por proposta, incluindo aplicação
em metade delas:

| Configuração, inserção por movimento | Integral | Incremental | Auto |
|---|---:|---:|---:|
| 200×200, alvo 2%, distância 100 | 2,36 | 2,66 | 2,84 |
| 1000×1000, alvo 2%, distância 500 | 78,09 | 54,08 | 53,05 |
| 1000×1000, alvo 20%, distância 500 | 454,66 | 46,49 | 47,46 |

No último exemplo, a razão entre integral e auto foi cerca de 9,6,
com calibração de 0,179 s compartilhável entre réplicas. Os resultados
também mostram limites: em 200×200, alvo 2%, inserção longa recebida
como solução completa, as medianas foram 2,51 µs, 3,06 µs e 6,65 µs,
respectivamente. A manutenção do estado misto, a validação/identificação
do intervalo e o custo de despacho podem superar o ganho em workloads
esparsos. O modo automático não garante vencer a melhor estratégia fixa.

O auto foi mais rápido que o integral em 102 dos 108 workloads medidos.
Esse número descreve este piloto, não uma taxa esperada de acerto do
seletor. As medições são curtas, em máquina compartilhada, sem intervalos
de confiança e com poucos tamanhos e matrizes. Não há alegação de
superioridade universal, nem de ganho de qualidade do PT.

Os CSVs finais são `results/auto_benchmark_raw.csv` e
`results/auto_benchmark_medians.csv`. O ambiente consta em
`results/environment_v2.json`: Linux x86-64, AMD EPYC 9V74, GCC 13.3.0,
uma thread por trecho cronometrado, sem `-march=native`, sem LTO e semente
20261030 para as matrizes/propostas. A ordem dos backends no piloto é
fixa; a ordem na calibração alterna entre repetições.

## Evidências incluídas

- `results/validation_release.log` e `validation_sanitizers.log`.
- `results/validation_summary.json`.
- `results/pt_integration_release.json` e `pt_integration_sanitizers.json`.
- `results/installer_validation.json`.
- `results/pt_precision.log` e `pt_construction.log`.
- CSVs e metadados do piloto final.

Os programas, geradores, fixtures e scripts necessários estão no pacote.
Binários dependentes da máquina e resultados históricos do repositório
recebido não fazem parte da distribuição.
