#include <stdio.h>

int main(int argc, char *argv[]) {
    //FILE *fopen("test_file.txt", "r");
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
    }
}
