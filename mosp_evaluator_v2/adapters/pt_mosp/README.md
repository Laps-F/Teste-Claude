# Adaptador opcional para o PT-MOSP fornecido

Este diretório contém as alterações para o repositório `PT-MOSP-main.zip`
enviado em 29/10/2026. O núcleo da biblioteca compila e funciona sem
esses arquivos. O instalador verifica hashes dos arquivos originais,
preserva backups e instala os fontes do núcleo em `MOSP/mosp_evaluator/`.
Não altera o ZIP original, resultados históricos ou dados de instâncias.

## Instalação

Extraia o ZIP do PT-MOSP em um diretório de trabalho e, a partir da raiz
da biblioteca, execute:

```sh
python3 scripts/install_pt_mosp.py --repo /caminho/PT-MOSP-main --check
python3 scripts/install_pt_mosp.py --repo /caminho/PT-MOSP-main
cd /caminho/PT-MOSP-main/MOSP
make -f mosp_evaluator.mk
```

O novo alvo é `mainMOSP_eval`. O alvo `make mainMOSP` do Makefile
original também recebe o vínculo com a biblioteca. O alvo original
`make all` continua incluindo comparadores que não vieram no ZIP;
use um dos alvos MOSP acima para compilar a integração.

Se algum arquivo não corresponder à versão original ou à já adaptada,
o instalador interrompe antes de alterar arquivos. Os originais alterados
ficam em `mosp_evaluator_backup/`, preservando os caminhos relativos.
Rodar novamente o instalador sobre a mesma instalação é idempotente.
`pt_mosp_v2.patch` permite revisar as alterações; o instalador é a rota
recomendada porque também verifica a versão e copia os novos fontes.

O PTAPI fornecido usa cabeçalhos específicos do GCC, incluindo
`bits/stdc++.h`; a integração foi testada com GCC 13.3.0/C++20 em Linux.
Essa exigência pertence ao PTAPI, enquanto o núcleo usa C++17 padrão.

## Exemplo

```sh
./mainMOSP_eval /caminho/instancia.txt \
  --EVALUATOR auto --ENERGY mmosp \
  --READ 1 --SEQUENCE 1 \
  --N_REPLICAS 11 --THREAD_USED 11 \
  --TEMP_INIT 0.01 --TEMP_FIM 10 \
  --MCL 600 --PTL 2000 --OUTDIR resultados_v2
```

Essas temperaturas reproduzem o intervalo padrão do programa e não
constituem uma recomendação calibrada para MMOSP. A energia modificada
introduz diferenças menores que uma unidade, que devem ser consideradas
ao ajustar a escada. O adaptador mantém as estratégias de atualização
de temperatura do PTAPI.

| Parâmetro | Interpretação |
|---|---|
| `--EVALUATOR full\|incremental\|auto` | Estratégia de cálculo; padrão `auto` |
| `--ENERGY mosp\|mmosp` | Energia do PT; padrão `mosp` |
| `--READ 1` | Corpo da matriz com padrões nas linhas |
| `--READ 0` | Corpo da matriz com padrões nas colunas |
| `--SEQUENCE 0` | Solução representa uma permutação de padrões |
| `--SEQUENCE 1` | Solução representa uma permutação de peças; padrão do repositório |
| `--AUDIT 1` | Confere cada proposta por uma avaliação integral independente; padrão 0 |

O cabeçalho é sempre `numero_de_padroes numero_de_pecas`, nos dois
valores de READ. As entradas seguem a regra original: somente 1 é
presença. As opções de temperatura, movimentos, iterações e threads
mantêm seus nomes originais. `MOV_TYPE=0/1/2` corresponde a troca,
inversão e inserção.

A inserção do código original usa destino antes da posição selecionada
e decrementa o destino quando a origem é menor. Essa regra foi mantida
no gerador do PT; o adaptador informa à biblioteca a posição final
efetivamente produzida. A biblioteca de uso geral usa sempre posição
final em `Move::insert(from,to)`.
O gerador original pode produzir movimento nulo ao selecionar o sucessor
imediato à direita; esse comportamento foi preservado. Propostas com
sequência de padrões idêntica à referência podem ser respondidas sem
executar um núcleo de avaliação, portanto os contadores de núcleos podem
ser menores que o número total de propostas auditadas.

## Arquitetura e correções necessárias

`Problem` passa a oferecer uma fábrica opcional de sessões de avaliação.
O comportamento padrão, para outros problemas, continua chamando a
avaliação anterior. Cada nó MCMC recebe sua sessão e chama seu método
de aplicação depois da aceitação e ao receber uma configuração de outro
nó. A sessão permanece no nó mesmo se outro trabalhador executar sua
próxima etapa.

O estado incremental não é armazenado em `solMOSP`. Assim, cópias de
candidatas, melhores soluções e trocas transportam a permutação e suas
métricas, sem copiar todos os prefixos/sufixos. Um identificador de
proposta diferencia a aceitação local de uma configuração recebida na
troca. Propostas por padrões usam os extremos conhecidos do movimento;
propostas por peças são decodificadas e avaliadas pela sequência completa
de padrões. Nas trocas, a sessão reconcilia seu cache com a nova solução.

As alterações que afetam a interpretação dos dados e da energia são:

- `NodeMCMC::getEnergia` retorna double, preservando a fração usada por
  NodeSwap e pelas estatísticas de temperatura.
- O MCNH recebe a mesma orientação da matriz que o leitor do PT.
- A conversão por peças é dinâmica, com desempate por ID e inclusão de
  padrões vazios; os bitsets fixos do código original deixam de ser o
  caminho do avaliador/decodificador.
- A construção de padrões inclui os padrões vazios que o sequenciamento
  original do MCNH deixava fora do trecho preenchido da solução.
- O objetivo MOSP original e a soma do perfil são armazenados além da energia.

O getter legado `getPatternBitsets()` continua disponível até 1000
peças. Ele lança erro acima desse tamanho; o avaliador e o decodificador
dinâmicos não usam esse getter. O MCNH mantém suas matrizes densas de
grafo e seu próprio custo de memória; esta entrega não o substitui.

Para a execução concorrente, os trabalhadores iniciam depois da montagem
do grafo de dependências e de todas as construções iniciais. O sinalizador
de término e o contador de nós são atômicos. O grafo passa a ter propriedade
explícita de seus nós e os destrói depois do término dos trabalhadores,
liberando também as sessões e estatísticas de troca. Esses ajustes estão
isolados no adaptador e não são dependências da biblioteca geral.

## Saídas

Os resultados ficam em `OUTDIR/PT/ENERGY_EVALUATOR/`, evitando colisões
entre configurações. O arquivo `_res.txt` mantém o MOSP inteiro na
terceira linha. O arquivo `_res.json` registra MOSP, soma do perfil,
MMOSP, energia usada, sequência, densidade, tempos, decisões de seleção
e contadores de avaliação/aplicação. A mesma informação é emitida em stdout.

O tempo de parede começa antes da construção do problema e inclui
leitura, calibração, construções e busca. O tempo do construtivo não
é acrescentado novamente a esse total. O campo de iteração da melhor
solução conserva a convenção original; -1 identifica uma solução inicial.
`--AUDIT 1` acrescenta conferências deliberadamente caras e deve ser
desativado para medir desempenho.

## Testes reproduzíveis

Depois de compilar, na raiz da biblioteca:

```sh
python3 adapters/pt_mosp/test_integration.py --repo /caminho/PT-MOSP-main
```

O script exercita 72 combinações de formato, representação, vizinhança,
energia e avaliador, mais quatro casos degenerados. Confere toda proposta
no programa e verifica a solução final por um oráculo Python de primeiras
e últimas ocorrências. São 37.632 propostas auditadas por rodada.

Há ainda `test_precision.cpp`, que testa o acesso fracionário à energia e
a troca real de soluções do NodeSwap, e `test_construction.cpp`, que
confere a mesma solução MCNH para a matriz e sua disposição transposta.
As instruções exatas e os resultados estão em `docs/VALIDACAO.md`.

As verificações usam o código real do PTAPI com matrizes sintéticas.
O adaptador mantém o uso de `random_device` do gerador original do PT-MOSP;
não acrescenta uma política global de sementes. A igualdade de trajetórias
sob a mesma semente foi verificada no exemplo controlado da biblioteca.
O ZIP recebido inclui resultados históricos, mas não inclui as instâncias
MOSP usadas naqueles experimentos. Não foi realizado um novo benchmark
de qualidade ou tempo total nessas instâncias.
