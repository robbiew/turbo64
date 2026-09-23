/* CONFIGURE UI - Common screen utilities. */
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

void ui_clear_screen(void)
{
  clrscr();
}

void ui_print_line(const char *text)
{
  printf("%s\n", text);
}

void ui_print_separator(void)
{
  u8 i;
  /* Built at runtime, not stored: a 40-char literal cost ~40 resident bytes
   * the SIEC editor build (9 bytes free) could not spare after the disk.c
   * DISK_OVER change grew disk_open. */
  for (i = 0; i < 40; i++) putchar('=');
  putchar('\n');
}

void ui_print_centered(const char *text)
{
  int len = strlen(text);
  int padding = (UI_SCREEN_WIDTH - len) / 2;
  int i;
  
  for (i = 0; i < padding; i++) {
    printf(" ");
  }
  printf("%s\n", text);
}

void ui_print_header_bar(const char *text)
{
  int len = (int)strlen(text);
  int padding = (UI_SCREEN_WIDTH - len) / 2;
  int i;

  if (padding < 0) padding = 0;

  textcolor(COLOR_LT_GREEN);
  revers(1);
  for (i = 0; i < padding; i++) putchar(' ');
  printf("%s", text);
  /* Fill the rest of the 40-col row so the whole line is a solid green bar.
   * Printing exactly UI_SCREEN_WIDTH chars wraps the cursor to the next line,
   * so no explicit newline is emitted here. */
  for (i = padding + len; i < UI_SCREEN_WIDTH; i++) putchar(' ');
  revers(0);
  textcolor(COLOR_WHITE);
}

void ui_screen_header(const char *title)
{
  ui_clear_screen();
  ui_print_header_bar(UI_APP_HDR);
  printf("\n");
  textcolor(COLOR_WHITE);
  ui_print_centered(title);
  printf("\n");
}

/* Yellow so the hotkey stands out against the green label. */
void ui_print_hotkey(char key)
{
  textcolor(COLOR_YELLOW);
  revers(1);
  putchar(key);
  revers(0);
}

void ui_hotkey_label(char key, const char *label)
{
  ui_print_hotkey(key);
  textcolor(COLOR_LT_GREEN);
  printf(" %s  ", label);
  textcolor(COLOR_WHITE);
}

void ui_beep(void)
{
  printf("\x07");
}

/* Edit-screen field label: " <key> LABEL   : " — reverse hotkey, green 8-wide
 * label, colon; leaves color white for the caller's value. Shared by the
 * message-area and door editors. */
void ui_edit_label(char key, const char *label)
{
  putchar(' ');
  ui_print_hotkey(key);
  textcolor(COLOR_LT_GREEN);
  printf(" %-8s: ", label);
  textcolor(COLOR_WHITE);
}

/* Backspace-aware unsigned-number entry; -1 if left blank, else 0..255. */
int ui_read_num(const char *prompt)
{
  char c[4] = { 0, 0, 0, 0 };
  int len = 0, v = 0, i;
  printf("%s", prompt);
  for (;;) {
    char ch = getch();
    if (ch == 13 || ch == '\n') { printf("\n"); break; }
    if ((ch == 20 || ch == 8) && len > 0) { len--; c[len] = 0; printf("\x14"); continue; }
    if (ch >= '0' && ch <= '9' && len < 3) { c[len++] = ch; putchar(ch); }
  }
  if (len == 0) return -1;
  for (i = 0; c[i]; i++) v = v * 10 + (c[i] - '0');
  return v > 255 ? 255 : v;
}
