CC = gcc
exec = dotk.out
exec_glob = dotk
sources_dir = src
out_dir = out
sources = $(wildcard $(sources_dir)/*.c)
objects = $(patsubst $(sources_dir)/%.c,$(out_dir)/%.o, $(sources)) 
flags = -g -lm #-O3
extra = -fsanitize=address -static-libasan 
all: dir $(exec)

$(exec): $(objects)
	$(CC) $^ $(flags) $(extra) -o $@

$(objects): $(out_dir)/%.o : $(sources_dir)/%.c
	$(CC) -c $(flags) $(extra) $< -o $@

rebuild:
	make clean
	make all

dir:
	mkdir -p $(out_dir)

install:
	make all
	cp ./$(exec) /usr/local/bin/$(exec_glob)

clean:
	-rm *.out
	-rm $(out_dir)/*.o
