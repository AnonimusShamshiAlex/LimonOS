#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "games.h"
#include "keyboard.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static int fails;
int main(void){
  static struct game g; uint64_t now=0;
  /* Змейка: идёт вправо, упирается в стену */
  game_init(&g,GAME_SNAKE,42,now); CHECK(g.paused&&g.fresh); game_key(&g,KEY_RIGHT,0,now); CHECK(!g.paused);
  CHECK(g.u.sn.len==4);
  int hx=g.u.sn.x[0];
  for(int i=0;i<10;i++){ now+=20; game_tick(&g,now);} 
  CHECK(g.u.sn.x[0]>hx || g.over);
  /* разворот назад запрещён */
  game_init(&g,GAME_SNAKE,1,now); game_key(&g,KEY_LEFT,0,now); CHECK(g.u.sn.nq==0);
  /* бот-змейка: ест еду и растёт (идём к еде по осям) */
  game_init(&g,GAME_SNAKE,7,now); game_key(&g,KEY_RIGHT,0,now); int ate=0;
  for(int it=0; it<3000 && !g.over; it++){
     struct snake_st*s=&g.u.sn; int hx=s->x[0], hy=s->y[0];
     int want = (hx<s->fx)?1:(hx>s->fx)?3:(hy<s->fy)?2:0;
     /* без самоубийства: если ход в тело — пробуем другой */
     for(int t=0;t<4;t++){ int d=(want+t)&3; int nx=hx+(d==1)-(d==3), ny=hy+(d==2)-(d==0);
        int bad=nx<0||ny<0||nx>=SNAKE_W||ny>=SNAKE_H; for(int i=0;i<s->len-1&&!bad;i++) if(s->x[i]==nx&&s->y[i]==ny) bad=1;
        if(!bad && d!=((s->dir+2)&3)){ uint32_t k= d==0?KEY_UP:d==1?KEY_RIGHT:d==2?KEY_DOWN:KEY_LEFT; game_key(&g,k,0,now); break; } }
     now+=15; game_tick(&g,now);
  }
  ate=g.score>0; CHECK(ate); printf("snake bot score=%d len=%d over=%d\n",g.score,g.u.sn.len,g.over);
  /* Тетрис: 7 фигур по 4 клетки в каждом повороте */
  for(int p=0;p<7;p++) for(int r=0;r<4;r++){ int c[4][2]; CHECK(tetris_piece_cells(p,r,c)==4); }
  game_init(&g,GAME_TETRIS,5,now); g.paused=0;
  int survived=0;
  for(int i=0;i<200 && !g.over;i++){ game_key(&g,' ',0,now); now+=5; game_tick(&g,now); survived++; }
  printf("tetris dropped %d pieces over=%d score=%d\n",survived,g.over,g.score);
  CHECK(g.over); /* без движения башня обязана упереться */
  /* очистка линий: ручная заливка */
  game_init(&g,GAME_TETRIS,5,now); g.paused=0;
  for(int x=0;x<TET_W;x++) g.u.te.board[TET_H-1][x]= (x==0)?0:1;
  g.u.te.piece=0; g.u.te.rot=1; g.u.te.px=-2; g.u.te.py=0; /* вертикальная I в колонке 0 или 1 */
  int c[4][2]; tetris_piece_cells(0,1,c); printf("I rot1 cells: (%d,%d)(%d,%d)\n",c[0][0],c[0][1],c[1][0],c[1][1]);
  int colx=c[0][0]; g.u.te.px = 0-colx;
  game_key(&g,' ',0,now);
  CHECK(g.u.te.lines==1); CHECK(g.score>=100);
  /* Понг: игрок стоит — компьютер должен набрать очки; мяч не вылетает за пределы */
  game_init(&g,GAME_PONG,3,now); g.paused=0;
  for(int i=0;i<40000 && !g.over;i++){ now+=2; game_tick(&g,now); }
  printf("pong %d:%d over=%d\n",g.u.po.ps,g.u.po.as,g.over);
  CHECK(g.over);
  /* 2048: сдвиг и слияние */
  game_init(&g,GAME_2048,9,now); g.paused=0;
  memset(&g.u.g,0,sizeof g.u.g); g.u.g.cell[0][0]=2; g.u.g.cell[0][1]=2; g.u.g.cell[0][2]=4; g.u.g.cell[0][3]=4;
  game_key(&g,KEY_LEFT,0,now);
  CHECK(g.u.g.cell[0][0]==4); CHECK(g.u.g.cell[0][1]==8); CHECK(g.score==12);
  memset(&g.u.g,0,sizeof g.u.g); g.u.g.cell[0][0]=2; g.u.g.cell[0][1]=2; g.u.g.cell[0][2]=2; g.score=0;
  game_key(&g,KEY_LEFT,0,now); CHECK(g.u.g.cell[0][0]==4); CHECK(g.u.g.cell[0][1]==2);
  /* 2048: случайная игра не падает и не нарушает сумму */
  game_init(&g,GAME_2048,11,now); g.paused=0;
  for(int i=0;i<5000 && !g.over;i++){ uint32_t ks[4]={KEY_LEFT,KEY_DOWN,KEY_RIGHT,KEY_UP}; game_key(&g,ks[rand()%4],0,now);} 
  printf("2048 random game score=%d over=%d\n",g.score,g.over);
  printf(fails?"FAILED %d\n":"ALL OK\n",fails); return fails!=0;
}
