#main : main.c
#	gcc $^ -o $@ -lm

main.out : main.cpp
	$(CXX) $^ -o $@ -pthread -I/usr/include/libxml2 -lm -lxml2 -lcurl -lyaml -ggdb `pkg-config --cflags --libs libnotify`

