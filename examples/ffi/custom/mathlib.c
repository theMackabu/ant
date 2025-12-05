#include <stdio.h>

int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

void greet(const char* name) {
    printf("Hello, %s!\n", name);
}

double divide(double a, double b) {
    if (b != 0.0) {
        return a / b;
    }
    return 0.0;
}