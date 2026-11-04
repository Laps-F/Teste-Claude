# Algoritmos, correção e custo

Considere n padrões, m itens e E incidências binárias. O padrão j contém
o conjunto B_j. A permutação pi contém todos os padrões uma vez. O perfil
h_t conta as pilhas abertas durante o processamento do padrão em t,
incluindo itens cuja primeira e última ocorrências estão nesse mesmo t.

## Avaliador integral independente do estado

`Evaluator` copia as contagens totais de ocorrências dos itens para um
vetor de ocorrências restantes e limpa as marcas de itens iniciados.
Para cada padrão da sequência:

1. Abre os itens que aparecem pela primeira vez, aumentando o total ativo.
2. Registra esse total como a altura no estágio atual e atualiza o máximo.
3. Decrementa as ocorrências restantes dos itens presentes e fecha aqueles
   que acabam de ter a última ocorrência.

Assim, a altura é medida depois das aberturas e antes dos fechamentos.
Um item de ocorrência única contribui uma vez no próprio estágio. Não
há máscara especial para esse caso. Um item ausente nunca é aberto.
As ocorrências da instância são contagens de padrões contendo o item,
calculadas internamente após deduplicação de suas incidências.

Custo de uma avaliação: O(E+n+m), incluindo a validação O(n) da permutação.
Memória de trabalho por avaliador: O(n+m). A inicialização da instância
esparsa ordena e deduplica as listas de cada padrão, além de construir
contagens e bitsets; esse preparo é feito uma única vez.

## Estado incremental

Use índices de posição 0 a n-1 e fronteiras de 0 a n:

- P[0] é vazio e P[t+1] = P[t] união B_pi[t].
- S[n] é vazio e S[t] = B_pi[t] união S[t+1].
- h[t] = popcount(P[t+1] interseção S[t]).

Um item pertence a P[t+1] exatamente quando já ocorreu até t; pertence
a S[t] exatamente quando ocorre em t ou depois. A interseção equivale
à condição `first <= t <= last`, o que fundamenta a equivalência com
o objetivo clássico do MOSP.

São mantidos bitsets de 64 bits, dimensionados em execução, e uma árvore
de segmentos de máximos sobre o perfil. A instância compartilha os
bitsets dos padrões e as listas esparsas somente para leitura. Nenhum
bit de padding é ligado, pois as entradas são validadas pelos IDs reais.

Se K=ceil(m/64), a construção do estado custa O(n(K+1)); o termo adicional
trata também m=0. A memória P/S é 2(n+1)K palavras de 8 bytes, além da
permutação, perfil, validação e árvore O(n). Não há pré-processamento
que retire padrões ou itens silenciosamente.

## Avaliação de um intervalo

Uma permutação candidata completa e válida difere da corrente em um
intervalo [l,r]. Fora desse intervalo, os padrões são idênticos. Como
ambas são permutações do mesmo conjunto, o multiconjunto de padrões no
intervalo também é idêntico. Logo, perfis anteriores a l e posteriores
a r permanecem iguais, assim como as fronteiras P[l] e S[r+1].

O mesmo argumento vale para uma inserção, troca ou inversão construída
internamente por `Move`. O núcleo não expõe uma operação pública que
aceite um intervalo arbitrário sem garantir essa condição.

O algoritmo constrói sufixos temporários do intervalo candidato a partir
de S[r+1]. Depois percorre o intervalo a partir de P[l], calculando as
novas alturas e seu máximo. Combina esse máximo com duas consultas de
máximo externo. A permutação, perfil, árvore e P/S correntes são preservados.
Os buffers temporários são reutilizados e só crescem quando necessário.

Para L=r-l+1, `evaluate(Move)` custa O(L(K+1)+log n). A sobrecarga que
recebe uma solução completa acrescenta O(n), por validar a permutação
e localizar as diferenças. Se a solução é idêntica, ela apenas valida
e compara a sequência, devolvendo o máximo já mantido.

## Aplicação sem política de aceitação

`apply` prepara e avalia novamente o intervalo, grava a permutação e as
alturas locais, recompõe os prefixos internos e copia os sufixos calculados.
Depois atualiza as folhas afetadas da árvore e somente seus ancestrais.
A atualização em lote da árvore custa O(L+log n); evita L subidas separadas.
O custo total mantém O(L(K+1)+log n), mais O(n) se a entrada for uma solução
completa. A repetição após a avaliação evita manter um token de proposta
que poderia ficar obsoleto. Na aceitação, esse custo é pago novamente,
e foi incluído no benchmark de cadeias com aplicação.

Não há teste de melhora em `apply`. A aceitação de empates e pioras é
decidida exclusivamente pelo PT ou por outro algoritmo hospedeiro.
Para desfazer um movimento conhecido, aplique `move.inverse()` sobre
o estado imediatamente posterior à sua aplicação. Não aplique a inversa
depois de outros movimentos sem reconstituir a ordem adequada.

## Troca adjacente

Se a=pi[t] e b=pi[t+1], após a troca somente P[t+1] e S[t+1] mudam:

- P'[t+1] = P[t] união B_b.
- S'[t+1] = B_a união S[t+2].
- h'[t] = popcount(P'[t+1] interseção S[t]).
- h'[t+1] = popcount(P[t+2] interseção S'[t+1]).

As demais alturas permanecem iguais. O caminho adjacente custa
O(K+log n), sem buffers de intervalo. Ele é selecionado automaticamente
para movimentos que trocam posições adjacentes e para soluções completas
cujo intervalo de diferenças tem comprimento dois.
`apply_adjacent(t)` expõe esse caminho para varreduras de inserções.

## Memória por réplica: exemplo

Para n=m=1000, K=16:

| Estrutura | Dados úteis aproximados |
|---|---:|
| Bitsets dos padrões, compartilhados | 128.000 bytes |
| P/S de uma réplica | 256.256 bytes |
| Sufixos temporários de uma réplica, após intervalo de comprimento 1000 | 128.128 bytes |
| Prefixo temporário de uma réplica | 128 bytes |
| Permutação e perfil correntes | 8.000 bytes |
| Candidata local e perfil temporário, comprimento 1000 | 8.000 bytes |
| Árvore, base 1024 | 8.192 bytes |
| Marcas para validar a permutação | 1.000 bytes |
| Soma corrente do perfil | 8 bytes |

Esses valores não incluem o objeto, capacidades excedentes dos vetores,
alocador ou listas esparsas compartilhadas; são estimativas das cargas
úteis, e não medições de RSS. Os buffers podem manter a maior capacidade
alocada durante a vida do objeto. O avaliador integral precisa de vetores
de trabalho lineares em n+m e evita P/S por réplica.

## Limites da escolha incremental

Movimentos curtos e matrizes com muitas incidências favorecem o uso
incremental. Um candidato sem localidade pode alterar quase toda a
sequência; em instâncias muito esparsas, o avaliador integral pode ser
competitivo. A versão 2 oferece modos explícitos e seleção automática calibrada no
equipamento de execução. O seletor usa medições por tipo de entrada e
comprimento do intervalo; os detalhes e seus limites estão no guia
`GUIA_PT_BR.md`.

Esta implementação foi construída a partir da representação simplificada
do pacote `MOSP_avaliacao_incremental_analise_e_prototipo_v1.zip`. Não
inclui nem modifica a biblioteca legada de quatro famílias de bitsets.
O objetivo é uma implementação standalone reutilizável; as conclusões
experimentais referem-se exclusivamente aos testes presentes neste pacote.


## Estatísticas e energia na versão 2

O perfil corrente também mantém S=soma h[t] em um inteiro de 64 bits.
A avaliação de um intervalo acumula S'=S+soma(h'[t]-h[t]) no mesmo laço
que calcula as alturas novas. A aplicação atualiza esse escalar junto
das alturas. Na troca adjacente são somente duas parcelas. Esse cálculo
não aumenta a ordem de complexidade dos núcleos apresentados acima.

A energia MMOSP é M+S/(nM), para M>0, e zero no caso sem pilhas abertas.
Para M>0, o termo adicional pertence a (0,1], garantindo prioridade
estrita do máximo inteiro. Mantendo M fixo, comparar energias equivale
a comparar S. Os dois inteiros permanecem disponíveis e não são
recuperados por truncamento da energia.

O seletor automático compartilha um plano imutável e possui um estado
por réplica. Um caminho integral escolhido para uma proposta não
invalida o estado incremental: a aplicação sempre o atualiza quando
esse estado existe. Candidatas rejeitadas não alteram a referência.
Nos testes, as duas estatísticas são confrontadas com um oráculo de
primeiras/últimas ocorrências, inclusive em trajetórias mistas.
