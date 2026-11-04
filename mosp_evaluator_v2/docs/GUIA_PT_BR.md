# Biblioteca MOSP Evaluator 2.0.0

O núcleo é uma biblioteca C++17 de uso geral, com instância imutável e
memória de trabalho por solução/réplica. Não depende do PT-MOSP. O pacote
também contém um adaptador opcional, verificado contra o repositório
`PT-MOSP-main.zip` fornecido pelo usuário.

## Entradas e retornos

| Momento | Informação |
|---|---|
| Construção esparsa | Número de itens e uma lista de itens por padrão |
| Construção densa | Dimensões, matriz em ordem de linhas, eixo dos padrões e regra de presença |
| Avaliação integral | Permutação completa de padrões |
| Construção do estado adaptativo | Instância e solução corrente; política `full`, `incremental` ou `auto`; plano compartilhado opcional |
| Avaliação de proposta | Solução completa ou `Move` com tipo, origem e destino |
| Aplicação | Mesma forma de entrada; aplicação incondicional após decisão do hospedeiro |
| Escolha da energia | `Energy::MOSP` ou `Energy::MMOSP` sobre as métricas retornadas |

IDs e posições são baseados em zero. Uma inserção coloca o padrão na
posição final `to`. A inversão inclui ambos os extremos. Toda solução
completa é validada como permutação. Tipos e posições de movimentos
também são validados. O número de padrões é derivado da incidência.
Contagens de ocorrências, perfil e estado incremental são calculados
internamente. Temperaturas e decisões de aceitação pertencem ao PT.

`Metrics` contém `max_open` inteiro, `total_open` em 64 bits e o número
de padrões. Os métodos `energy` e `mmosp` devolvem `double`. O máximo
continua disponível para comparação com referências e limites. A parte
inteira da energia MMOSP não deve ser usada para recuperar esse máximo:
o termo adicional pode valer exatamente um.

## MMOSP

Para alturas h[t], M=max h[t] e S=soma h[t], a energia é
G=M+S/(nM), quando M>0; define-se zero para M=0. Ela preserva a
prioridade do máximo e desempata soluções pelo total de pilhas no perfil.
Soluções com o mesmo máximo e total continuam empatadas.

A rotina integral acumula S durante sua passagem normal. O estado
incremental usa S'=S+soma(h'[t]-h[t]) no intervalo modificado. A troca
adjacente altera apenas duas parcelas. Não é necessário acrescentar
outra árvore ou reconstruir o perfil inteiro.

O mesmo G deve alimentar a aceitação local e as trocas de réplicas.
Uma alteração unitária em S, com M fixo, muda G em 1/(nM). A seleção
de temperaturas precisa considerar essa escala; o pacote não reajusta
automaticamente a escada de temperaturas. A melhora no objetivo M
decorrente dessa orientação adicional precisa ser medida em execuções
do algoritmo hospedeiro.

## Escolha automática

A densidade é Z/(nm), onde Z é o número de presenças binárias. A
estimativa inicial compara o trabalho integral Z+n+m com o trabalho
incremental proporcional a L(ceil(m/64)+1)+log n. Ela orienta a ordem
inicial dos testes. A escolha final utiliza medições curtas na máquina
de execução, separadas por tipo de entrada/movimento e comprimento do
intervalo. Assim, uma matriz esparsa ainda pode usar o caminho incremental
em movimentos curtos.

O plano usa até quatro comprimentos distintos: 2, min(16,n), max(2,n/4)
e n. Cada grupo usa seis propostas em três repetições por padrão,
alternando a ordem das estratégias. As propostas aceitas são prescritas
em metade dos passos. Para propostas por movimento, o custo integral
inclui a materialização da candidata. Ambos os caminhos medidos mantêm
uma referência incremental nas aplicações, como em um seletor misto.
O gerador de calibração é privado e sua semente padrão é 20261029.

Uma vantagem incremental inferior a 5% favorece o caminho integral.
Esses padrões são escolhas de engenharia, sem pretensão de limiar
universal. `CalibrationOptions` permite ajustar amostragem, repetições
e semente. O plano é congelado antes da busca; não há cronômetros ou
reaprendizado em cada avaliação.

As decisões e os tempos ficam em `SelectionPlan::decisions()`. Uma
tabela gravada pode ser reconstruída com `from_decisions`. A mesma
instância e o mesmo plano são compartilhados somente para leitura;
cada réplica possui seu `AdaptiveEvaluator`. Caso todas as decisões
usem avaliação integral, não se aloca estado incremental por réplica.

No plano misto, mesmo propostas avaliadas integralmente atualizam o
estado incremental ao serem aceitas. Essa manutenção custa tempo e
memória e pode reduzir o ganho do seletor em certos workloads. O plano
interpola linearmente os tempos entre os comprimentos calibrados. Tabelas
explícitas com tempos zero preservam decisões por bucket. A política
não garante a estratégia mais rápida para todos os comprimentos ou
fases da busca. Os modos forçados permanecem disponíveis para comparação.

O custo de calibração faz parte da inicialização. Em experimentos com
orçamento total de tempo, deve ser contabilizado no mesmo orçamento.
Calibre uma vez por instância e compartilhe o plano entre suas réplicas.

## Representação por peças

`ItemOrderDecoder` é um componente opcional da biblioteca. Ele transforma
uma permutação de itens em uma permutação de padrões: cada padrão é
emitido quando todos os seus itens tiverem sido visitados. O desempate
usa IDs crescentes. Padrões vazios são emitidos primeiro. A transformação
é exata para essa regra de representação, com memória de trabalho
O(n+m) por decodificador e dados inversos compartilhados O(Z+n+m).

Uma troca de duas peças pode reorganizar vários padrões. O intervalo
usado no avaliador deve ser identificado na sequência de padrões
decodificada. O adaptador do PT-MOSP faz essa distinção; os índices de
uma peça não são passados como posições de padrões.
Ordens distintas de peças podem gerar exatamente a mesma sequência de
padrões. Nesses casos, o perfil também é idêntico e a MMOSP mantém o empate.

## Uso com o PT-MOSP

O guia específico está em `adapters/pt_mosp/README.md`. O adaptador:

- Preserva a representação por peças e também atende à ordem direta de padrões.
- Mantém sessões por nó MCMC, incluindo quando o nó migra entre trabalhadores.
- Aplica o movimento local após aceitação e reconcilia o estado nas trocas.
- Usa `double` no acesso à energia pelas rotinas de troca.
- Mantém MOSP e energia modificada em campos distintos de saída.
- Repassa a mesma orientação da matriz ao construtivo MCNH e ao avaliador.

As permutações/snapshots do PT não carregam cópias dos prefixos e
sufixos. Nas trocas do PTAPI, o cache permanece no nó e é reconciliado
com a configuração recebida, com custo O(nK) no pior caso. O exemplo
genérico da biblioteca também demonstra a alternativa de trocar os
objetos completos de estado com `std::swap`.

## Compilação e distribuição

Use `make`, `make check` e, quando disponível, `make sanitize`. Para
incorporar os fontes, compile os dois arquivos em `src/` com C++17 e
adicione `include/` ao caminho de cabeçalhos. O núcleo não requer
OpenMP nem bibliotecas externas. Os testes concorrentes usam threads.

Os métodos inteiros de `Evaluator`/`IncrementalEvaluator` da v1 foram
preservados; os métodos de métricas são adicionais. Recompile os
consumidores para usar os novos layouts das classes. A licença MIT
do núcleo está em `LICENSE`; o adaptador preserva os termos do código
original e da MCNH.

As evidências em `results/` incluem regressões, sanitizers, exemplos,
auditoria no PTAPI real e o piloto sintético. Não houve benchmark de
qualidade ou desempenho total do PT nas instâncias experimentais do
usuário, que não vieram no repositório anexado.
