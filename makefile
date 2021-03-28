OPTIONS := -flto -O3 -Wall -Wextra -Wshadow -Werror

.PHONY: all clean debug dist test

all: muinit

clean:
	@rm -f muinit test/test_child

debug: OPTIONS += -g -DDEBUG -O0
debug: muinit

dist: OPTIONS += -static
dist: clean muinit
	@echo "Stripping muinit..."
	@strip muinit

test: OPTIONS += -g -DDEBUG
test: test/test.sh test/test_child muinit
	@echo "Running $@..."
	@bash $<

%: %.c
	@echo "Building $@..."
	@$(CC) $< -o $@ $(OPTIONS)
