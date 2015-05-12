#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "vi.h"
#include "kmap.h"

static char **led_kmap = kmap_def;

static char *keymap(char **kmap, int c)
{
	static char cs[4];
	cs[0] = c;
	return kmap[c] ? kmap[c] : cs;
}

/* map cursor horizontal position to terminal column number */
int led_pos(char *s, int pos)
{
	return dir_context(s) >= 0 ? pos : xcols - pos - 1;
}

char *led_keymap(int c)
{
	return c >= 0 ? keymap(led_kmap, c) : NULL;
}

static char *led_render(char *s0)
{
	int n, maxcol = 0;
	int *pos;	/* pos[i]: the screen position of the i-th character */
	int *off;	/* off[i]: the character at screen position i */
	char **chrs;	/* chrs[i]: the i-th character in s1 */
	char *s1;
	struct sbuf *out;
	int i;
	s1 = ren_translate(s0 ? s0 : "");
	chrs = uc_chop(s1, &n);
	pos = ren_position(s0);
	off = malloc(xcols * sizeof(off[0]));
	memset(off, 0xff, xcols * sizeof(off[0]));
	for (i = 0; i < n; i++) {
		int curpos = led_pos(s0, pos[i]);
		if (curpos >= 0 && curpos < xcols) {
			off[curpos] = i;
			if (curpos > maxcol)
				maxcol = curpos;
		}
	}
	out = sbuf_make();
	for (i = 0; i <= maxcol; i++) {
		if (off[i] >= 0 && uc_isprint(chrs[off[i]]))
			sbuf_mem(out, chrs[off[i]], uc_len(chrs[off[i]]));
		else
			sbuf_chr(out, ' ');
	}
	free(pos);
	free(off);
	free(chrs);
	free(s1);
	return sbuf_done(out);
}

void led_print(char *s, int row)
{
	char *r = led_render(s);
	term_pos(row, 0);
	term_kill();
	term_str(r);
	free(r);
}

static int led_lastchar(char *s)
{
	char *r = *s ? strchr(s, '\0') : s;
	if (r != s)
		r = uc_beg(s, r - 1);
	return r - s;
}

static int led_lastword(char *s)
{
	char *r = *s ? uc_beg(s, strchr(s, '\0') - 1) : s;
	int kind;
	while (r > s && uc_isspace(r))
		r = uc_beg(s, r - 1);
	kind = r > s ? uc_kind(r) : 0;
	while (r > s && uc_kind(uc_beg(s, r - 1)) == kind)
		r = uc_beg(s, r - 1);
	return r - s;
}

static void led_printparts(char *pref, char *main, char *post)
{
	struct sbuf *ln;
	int off, pos;
	ln = sbuf_make();
	sbuf_str(ln, pref);
	sbuf_str(ln, main);
	off = uc_slen(sbuf_buf(ln));
	sbuf_str(ln, post);
	/* cursor position for inserting the next character */
	if (post[0]) {
		pos = ren_cursor(sbuf_buf(ln), ren_pos(sbuf_buf(ln), off));
	} else {
		int len = sbuf_len(ln);
		sbuf_str(ln, keymap(led_kmap, 'a'));
		pos = ren_pos(sbuf_buf(ln), off);
		sbuf_cut(ln, len);
	}
	led_print(sbuf_buf(ln), -1);
	term_pos(-1, led_pos(sbuf_buf(ln), ren_cursor(sbuf_buf(ln), pos)));
	sbuf_free(ln);
}

static char *led_line(char *pref, char *post, int *key, char ***kmap)
{
	struct sbuf *sb;
	int c;
	sb = sbuf_make();
	if (!pref)
		pref = "";
	if (!post)
		post = "";
	while (1) {
		led_printparts(pref, sbuf_buf(sb), post);
		c = term_read(-1);
		switch (c) {
		case TERMCTRL('f'):
			*kmap = kmap_farsi;
			continue;
		case TERMCTRL('e'):
			*kmap = kmap_def;
			continue;
		case TERMCTRL('h'):
		case 127:
			if (sbuf_len(sb))
				sbuf_cut(sb, led_lastchar(sbuf_buf(sb)));
			break;
		case TERMCTRL('u'):
			sbuf_cut(sb, 0);
			break;
		case TERMCTRL('v'):
			sbuf_chr(sb, term_read(-1));
			break;
		case TERMCTRL('w'):
			if (sbuf_len(sb))
				sbuf_cut(sb, led_lastword(sbuf_buf(sb)));
			break;
		default:
			if (c == '\n' || c == TERMESC || c < 0)
				break;
			sbuf_str(sb, keymap(*kmap, c));
		}
		if (c == '\n' || c == TERMESC || c < 0)
			break;
	}
	*key = c;
	return sbuf_done(sb);
}

/* read an ex command */
char *led_prompt(char *pref, char *post)
{
	char **kmap = kmap_def;
	char *s;
	int key;
	s = led_line(pref, post, &key, &kmap);
	if (key == '\n')
		return s;
	free(s);
	return NULL;
}

/* read visual command input */
char *led_input(char *pref, char *post)
{
	struct sbuf *sb = sbuf_make();
	int key;
	while (1) {
		char *ln = led_line(pref, post, &key, &led_kmap);
		sbuf_str(sb, ln);
		if (key == '\n')
			sbuf_chr(sb, '\n');
		led_printparts(pref ? pref : "", ln, key == '\n' ? "" : post);
		if (key == '\n')
			term_chr('\n');
		pref = NULL;
		term_kill();
		free(ln);
		if (key != '\n')
			break;
	}
	if (key == TERMESC)
		return sbuf_done(sb);
	sbuf_free(sb);
	return NULL;
}
