#include <stdio.h>
#include <string.h>
#include "format.h"
int main(void){
  char b[200];
  ksnprintf(b,sizeof b,"[%.5s][%.47s][%-8s|][%8s][%.*s][%zu][%hhu][%5.2s]","abcdefghij","key.txt","ab","привет",3,"xyzxyz",(size_t)42,(unsigned)7,"hello");
  puts(b);
  return strcmp(b,"[abcde][key.txt][ab      |][  привет][xyz][42][7][   he]")!=0;
}
