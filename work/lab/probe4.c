#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "png.h"
struct je{jmp_buf j;};
static void ecb(png_structp p,png_const_charp m){fprintf(stderr,"E:%s\n",m);longjmp(((struct je*)png_get_error_ptr(p))->j,1);}
static void wcb(png_structp p,png_const_charp m){(void)p;}
int main(int c,char**v){
 FILE*f=fopen(v[1],"rb");struct je e;
 png_structp p=png_create_read_struct(PNG_LIBPNG_VER_STRING,&e,ecb,wcb);
 png_infop info=png_create_info_struct(p);
 if(setjmp(e.j))return 1;
 png_init_io(p,f);png_read_info(p,info);
 png_uint_32 w,h;int bd,ct,it;png_get_IHDR(p,info,&w,&h,&bd,&ct,&it,0,0);
 double fg; png_get_gAMA(p,info,&fg);
 double args[][2]={{2.2,1.0},{2.2,fg},{1.0,1.0}};
 for(int k=0;k<3;k++){
  fseek(f,0,0);
  png_destroy_read_struct(&p,&info,0);
  p=png_create_read_struct(PNG_LIBPNG_VER_STRING,&e,ecb,wcb);
  info=png_create_info_struct(p);
  if(setjmp(e.j))return 1;
  png_init_io(p,f);png_read_info(p,info);
  png_get_IHDR(p,info,&w,&h,&bd,&ct,&it,0,0);
  png_set_gamma(p,args[k][0],args[k][1]);
  png_read_update_info(p,info);
  size_t rb=png_get_rowbytes(p,info);png_bytep r=malloc(rb);
  png_read_row(p,r,0);
  printf("sg=%g fg=%g ->",args[k][0],args[k][1]);
  for(png_uint_32 x=0;x<4;x++)printf(" %d",r[3*x]);
  printf("\n");free(r);
 }
 fclose(f);return 0;}
