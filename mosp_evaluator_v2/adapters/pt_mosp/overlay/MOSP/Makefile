# Nome do executável
TARGET = mainMOSP
TARGET_BRKGA = mainBRKGA
TARGET_SND = mainSND
TARGET_SA = mainSA

# Pasta de código-fonte
SRC = mainMOSP.cpp
SRC_BRKGA = mainBRKGA.cpp BRKGA/mospDecoder.cpp BRKGA/Population.cpp
SRC_SND = SND/snd.cpp
SRC_SA = SA/sa.cpp

# Flags de compilação
CXX = g++ 
CXXFLAGS = ../PTAPI-main/include/*.h ./MOSP.cpp ./MCNH/*.h -std=c++20 -Wshadow -Wall -g -O3 -Wno-unused-result -lpthread -march=native
CXXFLAGS_BRKGA = -std=c++20 -fopenmp -Wshadow -Wall -g -O3 -Wno-unused-result -lpthread -march=native
CXXFLAGS_SND = -I. -std=c++20 -Wshadow -Wall -g -O3 -Wno-unused-result -lpthread -march=native
CXXFLAGS_SA = -I. -std=c++20 -Wshadow -Wall -g -O3 -Wno-unused-result -march=native

# Diretórios de instâncias
INST_DIR = ../Instances

# RESULT_DIR_A = resultados
RESULT_DIR = Results2
RESULT_DIR_A = resultados	
RESULT_DIR_B = resultados-challenge

# Parâmetros fixos
# PARAMS = --TEMP_INIT 0.01 --TEMP_FIM 10 --N_REPLICAS 5 --MCL 600 --TEMP_DIST 2 --MOV_TYPE 2 --TYPE_UPDATE 0 --THREAD_USED 5
PARAMS = --TEMP_INIT 0.01 --TEMP_FIM 10 --N_REPLICAS 11 --MCL 600 --PTL 2000 --TEMP_DIST 2 --MOV_TYPE 2 --TYPE_UPDATE 1 --TEMP_UPDATE 3

# Regra principal
all: $(TARGET) $(TARGET_BRKGA) $(TARGET_SND) $(TARGET_SA)

# Compilação
MOSP_EVALUATOR_FLAGS ?= -O3 -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
MOSP_EVALUATOR_SOURCES = mosp_evaluator/src/evaluator.cpp mosp_evaluator/src/adaptive.cpp

$(TARGET): $(SRC) MOSP.cpp MOSP.h $(MOSP_EVALUATOR_SOURCES) $(wildcard mosp_evaluator/include/mosp/*.hpp)
	$(CXX) $(MOSP_EVALUATOR_FLAGS) -Imosp_evaluator/include $(SRC) MOSP.cpp $(MOSP_EVALUATOR_SOURCES) -pthread -o $(TARGET)

$(TARGET_BRKGA): $(SRC_BRKGA)
	$(CXX) $(SRC_BRKGA) $(CXXFLAGS_BRKGA) -o $(TARGET_BRKGA) 

$(TARGET_SND): $(SRC_SND)
	$(CXX) $(SRC_SND) $(CXXFLAGS_SND) -o $(TARGET_SND) 

$(TARGET_SA): $(SRC_SA)
	$(CXX) $(SRC_SA) $(CXXFLAGS_SA) -o $(TARGET_SA)

run-all: $(TARGET)
	@echo "==> Iniciando execução de todas as instâncias..."
	@find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d | while read subdir; do \
		CREATOR_NAME=$$(basename "$$subdir"); \
		RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
		echo "--> Processando instâncias de: $$CREATOR_NAME"; \
		@mkdir -p "$$RESULT_SUBDIR"; \
		@find "$$subdir" -name '*.txt' | while read file; do \
			echo "    ==> Executando $$file 5 vezes..."; \
			@for i in $$(seq 0 4); do \
				echo "        --> Rodada $$i..."; \
				./$(TARGET) "$$file" $(PARAMS) --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
			done; \
		done; \
	done
	@echo "==> Execução de todas as instâncias foi concluída."

run-folder: $(TARGET)
	@if [ -z "$(FOLDER)" ]; then \
		echo "Erro: Especifique o diretório do criador."; \
		exit 1; \
	fi; \
	echo "DEBUG: FOLDER='$(FOLDER)'"; \
	CREATOR_NAME="$(FOLDER)"; \
	INST_SUBDIR=../Instances/"$$CREATOR_NAME"; \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
	if [ ! -d "$$INST_SUBDIR" ]; then \
		echo "Erro: Diretório de instância não encontrado: [$$INST_SUBDIR]"; \
		exit 1; \
	fi; \
	echo "==> Processando instâncias de: $$CREATOR_NAME (em $$INST_SUBDIR)"; \
	mkdir -p "$$RESULT_SUBDIR"; \
	for i in $$(seq 0 4); do \
		echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
		find "$$INST_SUBDIR" -name '*.txt' | while read file; do \
			echo "        --> Processando $$file (Rodada $$i)"; \
			./$(TARGET) "$$file" $(PARAMS) --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
		done; \
		echo "    ==> Rodada Mestra $$i concluída."; \
	done; \
	echo "==> Execução de $$CREATOR_NAME concluída."


run-one: $(TARGET)
	@if [ -z "$(FILE)" ]; then \
		echo "Uso: make run-one FILE=caminho/completo/para/instancia.txt"; \
		exit 1; \
	fi
	subdir=$$(dirname "$(FILE)"); \
	CREATOR_NAME=$$(basename "$$subdir"); \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"/temp; \
	mkdir -p "$$RESULT_SUBDIR"; \
	echo "==> Executando $(FILE) 5 vezes em modo de teste..."; \
	for i in $$(seq 0 4); do \
		echo "--> Rodada $$i..."; \
		./$(TARGET) "$(FILE)" $(PARAMS) --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
	done

# Regras para BRKGA
run-all-brkga: $(TARGET_BRKGA)
	@echo "==> Iniciando execução de todas as instâncias com BRKGA..."
	@ { \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d ! -name "Frinhani"; \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d -name "Frinhani"; \
	} | while read subdir; do \
		if [ -z "$$subdir" ]; then continue; fi; \
		CREATOR_NAME=$$(basename "$$subdir"); \
		RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
		echo "==> Processando instâncias de: $$CREATOR_NAME (em $$subdir) com BRKGA"; \
		mkdir -p "$$RESULT_SUBDIR"; \
		for i in $$(seq 0 4); do \
			echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
			find "$$subdir" -name '*.txt' | while read file; do \
				echo "        --> Processando $$file (Rodada $$i)"; \
				./$(TARGET_BRKGA) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
			done; \
			echo "    ==> Rodada Mestra $$i concluída."; \
		done; \
		echo "==> Execução de $$CREATOR_NAME com BRKGA concluída."; \
	done
	@echo "==> Execução de TODAS as instâncias com BRKGA foi concluída."

run-folder-brkga: $(TARGET_BRKGA)
	@if [ -z "$(FOLDER)" ]; then \
		echo "Erro: Especifique o diretório do criador."; \
		exit 1; \
	fi; \
	echo "DEBUG: FOLDER='$(FOLDER)'"; \
	CREATOR_NAME="$(FOLDER)"; \
	INST_SUBDIR=../Instances/"$$CREATOR_NAME"; \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
	if [ ! -d "$$INST_SUBDIR" ]; then \
		echo "Erro: Diretório de instância não encontrado: [$$INST_SUBDIR]"; \
		exit 1; \
	fi; \
	echo "==> Processando instâncias de: $$CREATOR_NAME (em $$INST_SUBDIR) com BRKGA"; \
	mkdir -p "$$RESULT_SUBDIR"; \
	for i in $$(seq 1 4); do \
		echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
		find "$$INST_SUBDIR" -name '*.txt' | sort -V | while read file; do \
			echo "        --> Processando $$file (Rodada $$i)"; \
			./$(TARGET_BRKGA) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
		done; \
		echo "    ==> Rodada Mestra $$i concluída."; \
	done; \
	echo "==> Execução de $$CREATOR_NAME com BRKGA concluída."

run-one-brkga: $(TARGET_BRKGA)
	@if [ -z "$(FILE)" ]; then \
		echo "Uso: make run-one-brkga FILE=caminho/completo/para/instancia.txt"; \
		exit 1; \
	fi
	subdir=$$(dirname "$(FILE)"); \
	CREATOR_NAME=$$(basename "$$subdir"); \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"/temp; \
	mkdir -p "$$RESULT_SUBDIR"; \
	echo "==> Executando $(FILE) 5 vezes em modo de teste com BRKGA..."; \
	for i in $$(seq 0 4); do \
		echo "--> Rodada $$i..."; \
		./$(TARGET_BRKGA) "$(FILE)" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
	done

# Regras para SND
run-all-snd: $(TARGET_SND)
	@echo "==> Iniciando execução de todas as instâncias com SND..."
	@ { \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d ! -name "Frinhani"; \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d -name "Frinhani"; \
	} | while read subdir; do \
		if [ -z "$$subdir" ]; then continue; fi; \
		CREATOR_NAME=$$(basename "$$subdir"); \
		RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
		echo "==> Processando instâncias de: $$CREATOR_NAME (em $$subdir) com SND"; \
		mkdir -p "$$RESULT_SUBDIR"; \
		for i in $$(seq 0 4); do \
			echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
			find "$$subdir" -name '*.txt' | while read file; do \
				echo "        --> Processando $$file (Rodada $$i)"; \
				./$(TARGET_SND) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
			done; \
			echo "    ==> Rodada Mestra $$i concluída."; \
		done; \
		echo "==> Execução de $$CREATOR_NAME com SND concluída."; \
	done
	@echo "==> Execução de TODAS as instâncias com SND foi concluída."

run-folder-snd: $(TARGET_SND)
	@if [ -z "$(FOLDER)" ]; then \
		echo "Erro: Especifique o diretório do criador."; \
		exit 1; \
	fi; \
	echo "DEBUG: FOLDER='$(FOLDER)'"; \
	CREATOR_NAME="$(FOLDER)"; \
	INST_SUBDIR=../Instances/"$$CREATOR_NAME"; \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
	if [ ! -d "$$INST_SUBDIR" ]; then \
		echo "Erro: Diretório de instância não encontrado: [$$INST_SUBDIR]"; \
		exit 1; \
	fi; \
	echo "==> Processando instâncias de: $$CREATOR_NAME (em $$INST_SUBDIR) com SND"; \
	mkdir -p "$$RESULT_SUBDIR"; \
	for i in $$(seq 5 9); do \
		echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
		find "$$INST_SUBDIR" -name '*.txt' | sort -V | while read file; do \
			echo "        --> Processando $$file (Rodada $$i)"; \
			./$(TARGET_SND) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
		done; \
		echo "    ==> Rodada Mestra $$i concluída."; \
	done; \
	echo "==> Execução de $$CREATOR_NAME com SND concluída."

run-one-snd: $(TARGET_SND)
	@if [ -z "$(FILE)" ]; then \
		echo "Uso: make run-one-snd FILE=caminho/completo/para/instancia.txt"; \
		exit 1; \
	fi
	subdir=$$(dirname "$(FILE)"); \
	CREATOR_NAME=$$(basename "$$subdir"); \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"/temp; \
	mkdir -p "$$RESULT_SUBDIR"; \
	echo "==> Executando $(FILE) 5 vezes em modo de teste com SND..."; \
	for i in $$(seq 0 4); do \
		echo "--> Rodada $$i..."; \
		./$(TARGET_SND) "$(FILE)" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
	done

run-all-sa: $(TARGET_SA)
	@echo "==> Iniciando execução de todas as instâncias com SA..."
	@ { \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d ! -name "Frinhani"; \
		find $(INST_DIR) -mindepth 1 -maxdepth 1 -type d -name "Frinhani"; \
	} | while read subdir; do \
		if [ -z "$$subdir" ]; then continue; fi; \
		CREATOR_NAME=$$(basename "$$subdir"); \
		RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
		echo "==> Processando instâncias de: $$CREATOR_NAME (em $$subdir) com SA"; \
		mkdir -p "$$RESULT_SUBDIR"; \
		for i in $$(seq 0 4); do \
			echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
			find "$$subdir" -name '*.txt' | while read file; do \
				echo "        --> Processando $$file (Rodada $$i)"; \
				./$(TARGET_SA) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
			done; \
			echo "    ==> Rodada Mestra $$i concluída."; \
		done; \
		echo "==> Execução de $$CREATOR_NAME com SA concluída."; \
	done
	@echo "==> Execução de TODAS as instâncias com SA foi concluída."

run-folder-sa: $(TARGET_SA)
	@if [ -z "$(FOLDER)" ]; then \
		echo "Erro: Especifique o diretório do criador."; \
		exit 1; \
	fi; \
	echo "DEBUG: FOLDER='$(FOLDER)'"; \
	CREATOR_NAME="$(FOLDER)"; \
	INST_SUBDIR=../Instances/"$$CREATOR_NAME"; \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"; \
	if [ ! -d "$$INST_SUBDIR" ]; then \
		echo "Erro: Diretório de instância não encontrado: [$$INST_SUBDIR]"; \
		exit 1; \
	fi; \
	echo "==> Processando instâncias de: $$CREATOR_NAME (em $$INST_SUBDIR) com SA"; \
	mkdir -p "$$RESULT_SUBDIR"; \
	for i in $$(seq 0 4); do \
		echo "    ==> Iniciando Rodada Mestra $$i (executando todos os arquivos 1x)..."; \
		find "$$INST_SUBDIR" -name '*.txt' | sort -V | while read file; do \
			echo "        --> Processando $$file (Rodada $$i)"; \
			./$(TARGET_SA) "$$file" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
		done; \
		echo "    ==> Rodada Mestra $$i concluída."; \
	done; \
	echo "==> Execução de $$CREATOR_NAME com SA concluída."

run-one-sa: $(TARGET_SA)
	@if [ -z "$(FILE)" ]; then \
		echo "Uso: make run-one-sa FILE=caminho/completo/para/instancia.txt"; \
		exit 1; \
	fi
	subdir=$$(dirname "$(FILE)"); \
	CREATOR_NAME=$$(basename "$$subdir"); \
	RESULT_SUBDIR="$(RESULT_DIR)/$$CREATOR_NAME"/temp; \
	mkdir -p "$$RESULT_SUBDIR"; \
	echo "==> Executando $(FILE) 5 vezes em modo de teste com SA..."; \
	for i in $$(seq 0 4); do \
		echo "--> Rodada $$i..."; \
		./$(TARGET_SA) "$(FILE)" --INST $$i --OUTDIR "$$RESULT_SUBDIR"; \
	done

# Limpa binários e resultados
clean:
	rm -f $(TARGET)
	rm -f $(TARGET_BRKGA)
	rm -f $(TARGET_SND)
	rm -f $(TARGET_SA)
	rm -f $(RESULT_DIR)/*.txt
	
.PHONY: all run-all run-folder run-one clean run-all-brkga run-folder-brkga run-one-brkga run-all-snd run-folder-snd run-one-snd run-all-sa run-folder-sa run-one-sa