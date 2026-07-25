#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "os.h"
#include "apps.h"
#include "sdfs.h"
#include "pscript.h"
#include "net.h"

const app_t apps[] = {
    {"TERM","Command shell + PicoScript",app_terminal,COL_LGREEN}, {"FILES","SD card file manager",app_files,COL_CYAN},
    {"EDIT","Text editor",app_editor,COL_AMBER}, {"NOTES","Markdown + Aksara Jawa",app_notes,COL_YELLOW},
    {"CALC","Calculator",app_calc,COL_WHITE}, {"CHAT","LLM chat over WiFi",app_chat,COL_CYAN},
    {"SNAKE","Classic snake game",app_snake,COL_GREEN}, {"BREAKOUT","Brick breaker",app_breakout,COL_ORANGE},
    {"INVADERS","Space invaders",app_invaders,COL_MAGENTA}, {"LIFE","Conway's Game of Life",app_life,COL_YELLOW},
    {"MONITOR","System monitor",app_monitor,COL_BLUE}, {"SETTINGS","Backlight, sound, power",app_settings,COL_GRAY},
    {"ABOUT","About this OS",app_about,COL_RED},
};
const int app_count = sizeof(apps) / sizeof(apps[0]);

static void boot_splash(void) {
    static const char *logo[] = {" ____  ____      _            ","|  _ \\|  _ \\ ___| |_ _ __ ___ ","| |_) | |_) / _ \\ __| '__/ _ \\","|  __/|  _ <  __/ |_| | | (_)|","|_|   |_| \\_\\___|\\__|_|  \\___|"};
    gfx_clear(GEM_WHITE); for(int i=0;i<5;i++) gfx_puts_at(16,100+i*8,logo[i],GEM_GREEN,GEM_WHITE);
    gfx_puts_at(72,160,"P R e t r o C a l c   O S   v 1 . 0",GEM_BLACK,GEM_WHITE);
    gfx_hline(60,175,200,GEM_GREEN); gfx_puts_at(72,260,"PRetroCalc OS  *  ESP32-S3  *  READY",GEM_DGRAY,GEM_WHITE); gfx_flush();
    static const note_t chime[]={{NOTE_C5,90},{NOTE_E5,90},{NOTE_G5,90},{NOTE_C6,130},{NOTE_G5,70},{NOTE_C6,70},{NOTE_E6,110},{NOTE_D6,60},{NOTE_C6,200},{NOTE_REST,40},{NOTE_G5,60},{NOTE_C6,220}};
    sound_play(chime,sizeof(chime)/sizeof(chime[0]));
}
static void launcher(void) {
    int sel=0; const int cols=3; absolute_time_t bat_refresh=get_absolute_time();
    for(;;) {
        os_gem_desktop_bg(); int bat=kbd_battery_percent(); char right[24]; snprintf(right,sizeof right,"BAT %3d%%",bat<0?0:bat);
        os_gem_menubar("PRetroCalc OS  Desk",right); int cx,cy,cw,ch; os_window("Applications",&cx,&cy,&cw,&ch);
        int cell_w=cw/cols,cell_h=62;
        for(int i=0;i<app_count;i++){int gx=cx+i%cols*cell_w,gy=cy+6+i/cols*cell_h,ix=gx+cell_w/2-12,iy=gy+4;bool cur=i==sel;
            if(cur)gfx_fill_rect(ix-6,iy-4,36,46,GEM_GREEN);gfx_fill_rect(ix,iy,24,24,GEM_WHITE);gfx_rect(ix,iy,24,24,GEM_BLACK);
            char initial[2]={apps[i].name[0],0};gfx_puts_at(ix+8,iy+8,initial,GEM_BLACK,GEM_WHITE);int tw=strlen(apps[i].name)*8;uint8_t fg=cur?GEM_WHITE:GEM_BLACK,bg=cur?GEM_GREEN:GEM_WHITE;
            gfx_fill_rect(gx+(cell_w-tw)/2-2,iy+30,tw+4,9,bg);gfx_puts_at(gx+(cell_w-tw)/2,iy+31,apps[i].name,fg,bg);}
        gfx_fill_rect(0,LCD_HEIGHT-12,LCD_WIDTH,12,GEM_WHITE);gfx_hline(0,LCD_HEIGHT-12,LCD_WIDTH,GEM_BLACK);
        gfx_puts_at(4,LCD_HEIGHT-10,"ARROWS=move ENTER=open T=terminal",GEM_BLACK,GEM_WHITE);gfx_puts_at(4,cy+ch-10,apps[sel].desc,GEM_DGRAY,GEM_WHITE);gfx_flush();
        kbd_event_t ev; bool have_event=false;
        while(!have_event){kbd_poll();sound_update();net_poll();if(time_reached(bat_refresh)){bat_refresh=make_timeout_time_ms(30000);break;}if(kbd_get_event(&ev)&&ev.type==KBD_EV_PRESS)have_event=true;sleep_ms(4);}
        if(!have_event)continue; sound_click();
        switch(ev.code){case KEY_LEFT:sel=(sel+app_count-1)%app_count;break;case KEY_RIGHT:sel=(sel+1)%app_count;break;case KEY_UP:sel=(sel+app_count-cols)%app_count;break;case KEY_DOWN:sel=(sel+cols)%app_count;break;case't':case'T':app_terminal();break;case KEY_ENTER:apps[sel].run();break;case KEY_POWER:kbd_power_off();break;}
    }
}
void pretro_main(void) {
    kbd_init(); gfx_init(); sound_init(); os_sd_present=sdfs_init();
    kbd_set_lcd_backlight(255); kbd_set_backlight(80); boot_splash();
    if(os_sd_present){static char auto_buf[4096];uint32_t len=0;if(sdfs_read_file("AUTORUN.PS",auto_buf,sizeof auto_buf,&len)){os_term_init();os_clear_screen();pscript_run(auto_buf);}}
    launcher();
}
