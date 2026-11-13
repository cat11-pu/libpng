#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "png.h"
struct je { jmp_buf j; };
static void ecb(png_structp p, png_const_charp m){ fprintf(stderr,"E:%s\n",m); longjmp(((struct je*)png_get_error_ptr(p))->j,1);}
static void wcb(png_structp p, png_const_charp m){(void)p;}
static void t(const char*path,double sg,double fg){
 FILE*f=fopen(path,"rb"); struct je e;
 png_structp p=png_create_read_struct(PNG_LIBPNG_VER_STRING,&e,ecb,wcb);
 png_infop info=png_create_info_struct(p);
 if(setjmp(e.j)){png_destroy_read_struct(&p,&info,0);fclose(f);return;}
 png_init_io(p,f); png_read_info(p,info);
 png_uint_32 w,h;int bd,ct,it; png_get_IHDR(p,info,&w,&h,&bd,&ct,&it,0,0);
 if(bd==16)png_set_strip_16(p);
 png_set_gamma(p,sg,fg);
 png_read_update_info(p,info);
 size_t rb=png_get_rowbytes(p,info); png_bytep*r=malloc(h*sizeof(void*));
 for(png_uint_32 y=0;y<h;y++)r[y]=malloc(rb);
 png_read_image(p,r);
 printf("sg=%g fg=%g ->",sg,fg);
 for(png_uint_32 x=0;x<4;x++)printf(" %d",r[0][3*x]);
 printf("\n");
 for(png_uint_32 y=0;y<h;y++)free(r[y]); free(r);
 png_destroy_read_struct(&p,&info,0); fclose(f);
}
int main(int c,char**v){
 t(v[1],2.2,0.45455);
 t(v[1],2.2,2.2);
 t(v[1],1.0,0.45455);
 return 0;}
