extern "C" int AntDesktop_Main(int argc, char **argv, void *stack_base);

int main(int argc, char **argv) {
  volatile char stack_base;
  return AntDesktop_Main(argc, argv, const_cast<char *>(&stack_base));
}
