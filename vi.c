/*
 * neatvi editor
 *
 * Copyright (C) 2015 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * This program is released under the Modified BSD license.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

static char vi_msg[EXLEN];	/* current message */
static char vi_charlast[8];	/* the last character searched via f, t, F, or T */
static int vi_charcmd;		/* the character finding command */
static int vi_arg1, vi_arg2;	/* the first and second arguments */
static int vi_ybuf;		/* current yank buffer */
static char *vi_kmap;		/* current insertion keymap */
static int vi_pcol;		/* the column requested by | command */
static int vi_printed;		/* ex_print() calls since the last command */
static int vi_scroll;		/* scroll amount for ^f and ^d*/

static void vi_wait(void)
{
	if (vi_printed > 1) {
		free(ex_read("[enter to continue]"));
		vi_msg[0] = '\0';
	}
	vi_printed = 0;
}

static void vi_drawmsg(void)
{
	led_print(vi_msg, xrows);
	vi_msg[0] = '\0';
}

/* redraw the screen */
static void vi_draw(int xcol)
{
	int i;
	term_record();
	for (i = xtop; i < xtop + xrows; i++) {
		char *s = lbuf_get(xb, i);
		led_print(s ? s : (i ? "~" : ""), i - xtop);
	}
	vi_drawmsg();
	term_pos(xrow, led_pos(lbuf_get(xb, i), xcol));
	term_commit();
}

/* update the screen by removing lines r1 to r2 before an input command */
static void vi_drawrm(int r1, int r2, int newln)
{
	r1 = MIN(MAX(r1, xtop), xtop + xrows);
	r2 = MIN(MAX(r2, xtop), xtop + xrows);
	term_pos(r1 - xtop, 0);
	term_room(r1 - r2 + newln);
}

static int vi_buf[128];
static int vi_buflen;

static int vi_read(void)
{
	return vi_buflen ? vi_buf[--vi_buflen] : term_read();
}

static void vi_back(int c)
{
	if (vi_buflen < sizeof(vi_buf))
		vi_buf[vi_buflen++] = c;
}

static char *vi_char(void)
{
	return led_read(&vi_kmap);
}

static char *vi_prompt(char *msg, char **kmap)
{
	term_pos(xrows, led_pos(msg, 0));
	term_kill();
	return led_prompt(msg, "", kmap);
}

/* read an ex input line */
char *ex_read(char *msg)
{
	struct sbuf *sb;
	char c;
	if (xled) {
		char *s = led_prompt(msg, "", &vi_kmap);
		if (s)
			term_chr('\n');
		return s;
	}
	sb = sbuf_make();
	while ((c = getchar()) != EOF && c != '\n')
		sbuf_chr(sb, c);
	if (c == EOF) {
		sbuf_free(sb);
		return NULL;
	}
	return sbuf_done(sb);
}

/* show an ex message */
void ex_show(char *msg)
{
	if (xvis) {
		snprintf(vi_msg, sizeof(vi_msg), "%s", msg);
	} else if (xled) {
		led_print(msg, -1);
		term_chr('\n');
	} else {
		printf("%s", msg);
	}
}

/* print an ex output line */
void ex_print(char *line)
{
	if (xvis) {
		vi_printed += line ? 1 : 2;
		if (line)
			snprintf(vi_msg, sizeof(vi_msg), "%s", line);
		if (line)
			led_print(line, -1);
		term_chr('\n');
	} else {
		if (line)
			ex_show(line);
	}
}

static int vi_yankbuf(void)
{
	int c = vi_read();
	if (c == '"')
		return vi_read();
	vi_back(c);
	return 0;
}

static int vi_prefix(void)
{
	int n = 0;
	int c = vi_read();
	if ((c >= '1' && c <= '9')) {
		while (isdigit(c)) {
			n = n * 10 + c - '0';
			c = vi_read();
		}
	}
	vi_back(c);
	return n;
}

static int vi_col2off(struct lbuf *lb, int row, int col)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_off(ln, col) : 0;
}

static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

static int vi_nextoff(struct lbuf *lb, int dir, int *row, int *off)
{
	int o = *off + dir;
	if (o < 0 || !lbuf_get(lb, *row) || o >= uc_slen(lbuf_get(lb, *row)))
		return 1;
	*off = o;
	return 0;
}

static int vi_nextcol(struct lbuf *lb, int dir, int *row, int *off)
{
	char *ln = lbuf_get(lb, *row);
	int col = ln ? ren_pos(ln, *off) : 0;
	int o = ln ? ren_next(ln, col, dir) : -1;
	if (o < 0)
		return -1;
	*off = ren_off(ln, o);
	return 0;
}

static int vi_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *row, int *off)
{
	strcpy(vi_charlast, cs);
	vi_charcmd = cmd;
	return lbuf_findchar(lb, cs, cmd, n, row, off);
}

static int vi_search(int cmd, int cnt, int *row, int *off)
{
	int r = *row;
	int o = *off;
	int failed = 0;
	int len = 0;
	int i, dir;
	char *soff = "";
	if (cmd == '/' || cmd == '?') {
		char sign[4] = {cmd};
		char *kw = vi_prompt(sign, &vi_kmap);
		if (!kw)
			return 1;
		xfinddir = cmd == '/' ? +1 : -1;
		if (kw[0])
			snprintf(xfindkwd, sizeof(xfindkwd), "%s", kw);
		if (strchr(xfindkwd, cmd)) {
			soff = strchr(xfindkwd, cmd) + 1;
			*strchr(xfindkwd, cmd) = '\0';
		}
		free(kw);
	}
	dir = cmd == 'N' ? -xfinddir : xfinddir;
	if (!xfindkwd[0] || !lbuf_len(xb))
		return 1;
	o = *off;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, xfindkwd, dir, &r, &o, &len)) {
			failed = 1;
			break;
		}
		if (i + 1 < cnt && cmd == '/')
			o += len;
	}
	if (!failed) {
		*row = r;
		*off = o;
		while (soff[0] && isspace((unsigned char) soff[0]))
			soff++;
		if (soff[0]) {
			*off = -1;
			if (*row + atoi(soff) < 0 || *row + atoi(soff) >= lbuf_len(xb))
				failed = 1;
			else
				*row += atoi(soff);
		}
	}
	if (failed)
		snprintf(vi_msg, sizeof(vi_msg), "\"%s\" not found\n", xfindkwd);
	return failed;
}

/* read a line motion */
static int vi_motionln(int *row, int cmd)
{
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	int c = vi_read();
	int mark, mark_row, mark_off;
	switch (c) {
	case '\n':
	case '+':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case '-':
		*row = MAX(*row - cnt, 0);
		break;
	case '_':
		*row = MIN(*row + cnt - 1, lbuf_len(xb) - 1);
		break;
	case '\'':
		if ((mark = vi_read()) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, &mark_row, &mark_off))
			return -1;
		*row = mark_row;
		break;
	case 'j':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case 'k':
		*row = MAX(*row - cnt, 0);
		break;
	case 'G':
		*row = (vi_arg1 || vi_arg2) ? cnt - 1 : lbuf_len(xb) - 1;
		break;
	case 'H':
		if (lbuf_len(xb))
			*row = MIN(xtop + cnt - 1, lbuf_len(xb) - 1);
		else
			*row = 0;
		break;
	case 'L':
		if (lbuf_len(xb))
			*row = MIN(xtop + xrows - 1 - cnt + 1, lbuf_len(xb) - 1);
		else
			*row = 0;
		break;
	case 'M':
		if (lbuf_len(xb))
			*row = MIN(xtop + xrows / 2, lbuf_len(xb) - 1);
		else
			*row = 0;
		break;
	default:
		if (c == cmd) {
			*row = MAX(0, MIN(*row + cnt - 1, lbuf_len(xb) - 1));
			break;
		}
		if (c == '%' && (vi_arg1 || vi_arg2)) {
			if (cnt > 100)
				return -1;
			*row = MAX(0, lbuf_len(xb) - 1) * cnt / 100;
			break;
		}
		vi_back(c);
		return 0;
	}
	return c;
}

static char *vi_curword(struct lbuf *lb, int row, int off)
{
	struct sbuf *sb;
	char *ln = lbuf_get(lb, row);
	char *beg, *end;
	if (!ln)
		return NULL;
	beg = uc_chr(ln, ren_noeol(ln, off));
	end = beg;
	while (*end && uc_kind(end) == 1)
		end = uc_next(end);
	while (beg > ln && uc_kind(uc_beg(ln, beg - 1)) == 1)
		beg = uc_beg(ln, beg - 1);
	if (beg >= end)
		return NULL;
	sb = sbuf_make();
	sbuf_str(sb, "\\<");
	sbuf_mem(sb, beg, end - beg);
	sbuf_str(sb, "\\>");
	return sbuf_done(sb);
}

/* read a motion */
static int vi_motion(int *row, int *off)
{
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	char *ln = lbuf_get(xb, *row);
	int dir = dir_context(ln ? ln : "");
	int mark, mark_row, mark_off;
	char *cs;
	int mv;
	int i;
	if ((mv = vi_motionln(row, 0))) {
		*off = -1;
		return mv;
	}
	mv = vi_read();
	switch (mv) {
	case 'f':
		if (!(cs = vi_char()))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'F':
		if (!(cs = vi_char()))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case ';':
		if (!vi_charlast[0])
			return -1;
		if (vi_findchar(xb, vi_charlast, vi_charcmd, cnt, row, off))
			return -1;
		break;
	case ',':
		if (!vi_charlast[0])
			return -1;
		if (vi_findchar(xb, vi_charlast, vi_charcmd, -cnt, row, off))
			return -1;
		break;
	case 'h':
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(xb, -1 * dir, row, off))
				break;
		break;
	case 'l':
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(xb, +1 * dir, row, off))
				break;
		break;
	case 't':
		if (!(cs = vi_char()))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'T':
		if (!(cs = vi_char()))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'B':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 1, -1, row, off))
				break;
		break;
	case 'E':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 1, +1, row, off))
				break;
		break;
	case 'W':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, 1, +1, row, off))
				break;
		break;
	case 'b':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 0, -1, row, off))
				break;
		break;
	case 'e':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 0, +1, row, off))
				break;
		break;
	case 'w':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, 0, +1, row, off))
				break;
		break;
	case '{':
		for (i = 0; i < cnt; i++)
			if (lbuf_paragraphbeg(xb, -1, row, off))
				break;
		break;
	case '}':
		for (i = 0; i < cnt; i++)
			if (lbuf_paragraphbeg(xb, +1, row, off))
				break;
		break;
	case '[':
		if (vi_read() != '[')
			return -1;
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, -1, row, off))
				break;
		break;
	case ']':
		if (vi_read() != ']')
			return -1;
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, +1, row, off))
				break;
		break;
	case '0':
		*off = 0;
		break;
	case '^':
		*off = lbuf_indents(xb, *row);
		break;
	case '$':
		*off = lbuf_eol(xb, *row);
		break;
	case '|':
		*off = vi_col2off(xb, *row, cnt - 1);
		vi_pcol = cnt - 1;
		break;
	case '/':
		if (vi_search(mv, cnt, row, off))
			return -1;
		break;
	case '?':
		if (vi_search(mv, cnt, row, off))
			return -1;
		break;
	case 'n':
		if (vi_search(mv, cnt, row, off))
			return -1;
		break;
	case 'N':
		if (vi_search(mv, cnt, row, off))
			return -1;
		break;
	case TK_CTL('a'):
		if (!(cs = vi_curword(xb, *row, *off)))
			return -1;
		strcpy(xfindkwd, cs);
		free(cs);
		xfinddir = +1;
		if (vi_search('n', cnt, row, off))
			return -1;
		break;
	case ' ':
		for (i = 0; i < cnt; i++)
			if (vi_nextoff(xb, +1, row, off))
				break;
		break;
	case 127:
	case TK_CTL('h'):
		for (i = 0; i < cnt; i++)
			if (vi_nextoff(xb, -1, row, off))
				break;
		break;
	case '`':
		if ((mark = vi_read()) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, &mark_row, &mark_off))
			return -1;
		*row = mark_row;
		*off = mark_off;
		break;
	case '%':
		if (lbuf_pair(xb, row, off))
			return -1;
		break;
	default:
		vi_back(mv);
		return 0;
	}
	return mv;
}

static void swap(int *a, int *b)
{
	int t = *a;
	*a = *b;
	*b = t;
}

static char *lbuf_region(struct lbuf *lb, int r1, int o1, int r2, int o2)
{
	struct sbuf *sb;
	char *s1, *s2, *s3;
	if (r1 == r2)
		return uc_sub(lbuf_get(lb, r1), o1, o2);
	sb = sbuf_make();
	s1 = uc_sub(lbuf_get(lb, r1), o1, -1);
	s3 = uc_sub(lbuf_get(lb, r2), 0, o2);
	s2 = lbuf_cp(lb, r1 + 1, r2);
	sbuf_str(sb, s1);
	sbuf_str(sb, s2);
	sbuf_str(sb, s3);
	free(s1);
	free(s2);
	free(s3);
	return sbuf_done(sb);
}

static void vi_yank(int r1, int o1, int r2, int o2, int lnmode)
{
	char *region;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	reg_put(vi_ybuf, region, lnmode);
	free(region);
	xrow = r1;
	xoff = lnmode ? xoff : o1;
}

static void vi_delete(int r1, int o1, int r2, int o2, int lnmode)
{
	char *pref, *post;
	char *region;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	reg_put(vi_ybuf, region, lnmode);
	free(region);
	pref = lnmode ? uc_dup("") : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	lbuf_rm(xb, r1, r2 + 1);
	if (!lnmode) {
		struct sbuf *sb = sbuf_make();
		sbuf_str(sb, pref);
		sbuf_str(sb, post);
		lbuf_put(xb, r1, sbuf_buf(sb));
		sbuf_free(sb);
	}
	xrow = r1;
	xoff = lnmode ? lbuf_indents(xb, xrow) : o1;
	free(pref);
	free(post);
}

static int linecount(char *s)
{
	int n;
	for (n = 0; s; n++)
		if ((s = strchr(s, '\n')))
			s++;
	return n;
}

static int indentscopy(char *d, char *s, int len)
{
	int i;
	for (i = 0; i < len - 1 && (s[i] == ' ' || s[i] == '\t'); i++)
		d[i] = s[i];
	d[i] = '\0';
	return i;
}

static char *vi_input(char *pref, char *post, int *row, int *off)
{
	char ai[64] = "";
	char *rep, *s;
	struct sbuf *sb;
	int last;
	if (xai)
		pref += indentscopy(ai, pref, sizeof(ai));
	rep = led_input(pref, post, ai, xai ? sizeof(ai) - 1 : 0, &vi_kmap);
	if (!rep)
		return NULL;
	sb = sbuf_make();
	sbuf_str(sb, ai);
	sbuf_str(sb, pref);
	sbuf_str(sb, rep);
	s = sbuf_buf(sb);
	last = uc_lastline(s) - s;
	*off = MAX(0, uc_slen(sbuf_buf(sb) + last) - 1);
	if (last)
		while (xai && (post[0] == ' ' || post[0] == '\t'))
			post++;
	sbuf_str(sb, post);
	*row = linecount(sbuf_buf(sb)) - 1;
	free(rep);
	return sbuf_done(sb);
}

static char *vi_indents(char *ln)
{
	struct sbuf *sb = sbuf_make();
	while (xai && ln && (*ln == ' ' || *ln == '\t'))
		sbuf_chr(sb, *ln++);
	return sbuf_done(sb);
}

static void vi_change(int r1, int o1, int r2, int o2, int lnmode)
{
	char *region;
	int row, off;
	char *rep;
	char *pref, *post;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	reg_put(vi_ybuf, region, lnmode);
	free(region);
	pref = lnmode ? vi_indents(lbuf_get(xb, r1)) : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	vi_drawrm(r1, r2, 0);
	rep = vi_input(pref, post, &row, &off);
	if (rep) {
		lbuf_rm(xb, r1, r2 + 1);
		lbuf_put(xb, r1, rep);
		xrow = r1 + row - 1;
		xoff = off;
		free(rep);
	}
	free(pref);
	free(post);
}

static void vi_case(int r1, int o1, int r2, int o2, int lnmode, int cmd)
{
	char *pref, *post;
	char *region, *s;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	s = region;
	while (*s) {
		int c = (unsigned char) s[0];
		if (c <= 0x7f) {
			if (cmd == 'u')
				s[0] = tolower(c);
			if (cmd == 'U')
				s[0] = toupper(c);
			if (cmd == '~')
				s[0] = islower(c) ? toupper(c) : tolower(c);
		}
		s = uc_next(s);
	}
	pref = lnmode ? uc_dup("") : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	lbuf_rm(xb, r1, r2 + 1);
	if (!lnmode) {
		struct sbuf *sb = sbuf_make();
		sbuf_str(sb, pref);
		sbuf_str(sb, region);
		sbuf_str(sb, post);
		lbuf_put(xb, r1, sbuf_buf(sb));
		sbuf_free(sb);
	} else {
		lbuf_put(xb, r1, region);
	}
	xrow = r2;
	xoff = lnmode ? lbuf_indents(xb, r2) : o2;
	free(region);
	free(pref);
	free(post);
}

static void vi_pipe(int r1, int r2)
{
	char *text;
	char *rep;
	char *kmap = NULL;
	char *cmd = vi_prompt("!", &kmap);
	if (!cmd)
		return;
	text = lbuf_cp(xb, r1, r2 + 1);
	rep = cmd_pipe(cmd, text, 1, 1);
	if (rep) {
		lbuf_rm(xb, r1, r2 + 1);
		lbuf_put(xb, r1, rep);
	}
	free(cmd);
	free(text);
	free(rep);
}

static void vi_shift(int r1, int r2, int dir)
{
	struct sbuf *sb;
	char *ln;
	int i;
	for (i = r1; i <= r2; i++) {
		if (!(ln = lbuf_get(xb, i)))
			continue;
		sb = sbuf_make();
		if (dir > 0)
			sbuf_chr(sb, '\t');
		else
			ln = ln[0] == ' ' || ln[0] == '\t' ? ln + 1 : ln;
		sbuf_str(sb, ln);
		lbuf_rm(xb, i, i + 1);
		lbuf_put(xb, i, sbuf_buf(sb));
		sbuf_free(sb);
	}
	xrow = r1;
	xoff = lbuf_indents(xb, xrow);
}

static int vc_motion(int cmd)
{
	int r1 = xrow, r2 = xrow;	/* region rows */
	int o1 = xoff, o2 = xoff;	/* visual region columns */
	int lnmode = 0;			/* line-based region */
	int mv;
	vi_arg2 = vi_prefix();
	if (vi_arg2 < 0)
		return 1;
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	o2 = o1;
	if ((mv = vi_motionln(&r2, cmd))) {
		o2 = -1;
	} else if (!(mv = vi_motion(&r2, &o2))) {
		vi_read();
		return 1;
	}
	if (mv < 0)
		return 1;
	lnmode = o2 < 0;
	if (lnmode) {
		o1 = 0;
		o2 = lbuf_eol(xb, r2);
	}
	if (r1 > r2) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	}
	if (r1 == r2 && o1 > o2)
		swap(&o1, &o2);
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	if (!lnmode && strchr("fFtTeE%", mv))
		if (o2 < lbuf_eol(xb, r2))
			o2 = ren_noeol(lbuf_get(xb, r2), o2) + 1;
	if (cmd == 'y')
		vi_yank(r1, o1, r2, o2, lnmode);
	if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	if (cmd == 'c')
		vi_change(r1, o1, r2, o2, lnmode);
	if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	if (cmd == '!')
		vi_pipe(r1, r2);
	if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1);
	return 0;
}

static int vc_insert(int cmd)
{
	char *pref, *post;
	char *ln = lbuf_get(xb, xrow);
	int row, off = 0;
	char *rep;
	if (cmd == 'I')
		xoff = lbuf_indents(xb, xrow);
	if (cmd == 'A')
		xoff = lbuf_eol(xb, xrow);
	xoff = ren_noeol(ln, xoff);
	if (cmd == 'o')
		xrow += 1;
	if (cmd == 'i' || cmd == 'I')
		off = xoff;
	if (cmd == 'a' || cmd == 'A')
		off = xoff + 1;
	pref = ln && cmd != 'o' && cmd != 'O' ? uc_sub(ln, 0, off) : vi_indents(ln);
	post = ln && cmd != 'o' && cmd != 'O' ? uc_sub(ln, off, -1) : uc_dup("\n");
	vi_drawrm(xrow, xrow, cmd == 'o' || cmd == 'O');
	rep = vi_input(pref, post, &row, &off);
	if ((cmd == 'o' || cmd == 'O') && !lbuf_len(xb))
		lbuf_put(xb, 0, "\n");
	if (rep) {
		if (cmd != 'o' && cmd != 'O')
			lbuf_rm(xb, xrow, xrow + 1);
		lbuf_put(xb, xrow, rep);
		xrow += row - 1;
		xoff = off;
		free(rep);
	}
	free(pref);
	free(post);
	return !rep;
}

static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg1);
	int lnmode;
	char *buf = reg_get(vi_ybuf, &lnmode);
	int i;
	if (!buf) {
		snprintf(vi_msg, sizeof(vi_msg), "yank buffer empty\n");
		return 1;
	}
	if (lnmode) {
		struct sbuf *sb = sbuf_make();
		for (i = 0; i < cnt; i++)
			sbuf_str(sb, buf);
		if (!lbuf_len(xb))
			lbuf_put(xb, 0, "\n");
		if (cmd == 'p')
			xrow++;
		lbuf_put(xb, xrow, sbuf_buf(sb));
		xoff = lbuf_indents(xb, xrow);
		sbuf_free(sb);
	} else {
		struct sbuf *sb = sbuf_make();
		char *ln = xrow < lbuf_len(xb) ? lbuf_get(xb, xrow) : "\n";
		int off = ren_noeol(ln, xoff) + (ln[0] != '\n' && cmd == 'p');
		char *s = uc_sub(ln, 0, off);
		sbuf_str(sb, s);
		free(s);
		for (i = 0; i < cnt; i++)
			sbuf_str(sb, buf);
		s = uc_sub(ln, off, -1);
		sbuf_str(sb, s);
		free(s);
		lbuf_rm(xb, xrow, xrow + 1);
		lbuf_put(xb, xrow, sbuf_buf(sb));
		xoff = off + uc_slen(buf) * cnt - 1;
		sbuf_free(sb);
	}
	return 0;
}

static int join_spaces(char *prev, char *next)
{
	int prevlen = strlen(prev);
	if (!prev[0])
		return 0;
	if (prev[prevlen - 1] == ' ' || next[0] == ')')
		return 0;
	return prev[prevlen - 1] == '.' ? 2 : 1;
}

static int vc_join(void)
{
	struct sbuf *sb;
	int cnt = vi_arg1 <= 1 ? 2 : vi_arg1;
	int beg = xrow;
	int end = xrow + cnt;
	int off = 0;
	int i;
	if (!lbuf_get(xb, beg) || !lbuf_get(xb, end - 1))
		return 1;
	sb = sbuf_make();
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		char *lnend = strchr(ln, '\n');
		int spaces;
		if (i > beg)
			while (ln[0] == ' ' || ln[0] == '\t')
				ln++;
		spaces = i > beg ? join_spaces(sbuf_buf(sb), ln) : 0;
		off = uc_slen(sbuf_buf(sb));
		while (spaces--)
			sbuf_chr(sb, ' ');
		sbuf_mem(sb, ln, lnend - ln);
	}
	sbuf_chr(sb, '\n');
	lbuf_rm(xb, beg, end);
	lbuf_put(xb, beg, sbuf_buf(sb));
	xoff = off;
	sbuf_free(sb);
	return 0;
}

static int vi_scrollforeward(int cnt)
{
	if (xtop >= lbuf_len(xb) - 1)
		return 1;
	xtop = MIN(lbuf_len(xb) - 1, xtop + cnt);
	xrow = MAX(xrow, xtop);
	return 0;
}

static int vi_scrollbackward(int cnt)
{
	if (xtop == 0)
		return 1;
	xtop = MAX(0, xtop - cnt);
	xrow = MIN(xrow, xtop + xrows - 1);
	return 0;
}

static void vc_status(void)
{
	int col = vi_off2col(xb, xrow, xoff);
	snprintf(vi_msg, sizeof(vi_msg),
		"\"%s\"%c %d lines  L%d C%d\n",
		ex_path()[0] ? ex_path() : "unnamed",
		lbuf_modified(xb) ? '*' : ' ',
		lbuf_len(xb), xrow + 1,
		ren_cursor(lbuf_get(xb, xrow), col) + 1);
}

static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg1);
	char *cs = vi_char();
	char *ln = lbuf_get(xb, xrow);
	struct sbuf *sb;
	char *pref, *post;
	char *s;
	int off, i;
	if (!ln || !cs)
		return 1;
	off = ren_noeol(ln, xoff);
	s = uc_chr(ln, off);
	for (i = 0; s[0] != '\n' && i < cnt; i++)
		s = uc_next(s);
	if (i < cnt)
		return 1;
	pref = uc_sub(ln, 0, off);
	post = uc_sub(ln, off + cnt, -1);
	sb = sbuf_make();
	sbuf_str(sb, pref);
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs);
	sbuf_str(sb, post);
	lbuf_rm(xb, xrow, xrow + 1);
	lbuf_put(xb, xrow, sbuf_buf(sb));
	off += cnt - 1;
	xoff = off;
	sbuf_free(sb);
	free(pref);
	free(post);
	return 0;
}

static char rep_cmd[4096];	/* the last command */
static int rep_len;

static void vc_repeat(void)
{
	term_push(rep_cmd, rep_len);
}

static void vc_execute(void)
{
	static int exec_buf;
	int lnmode;
	int c = vi_read();
	char *buf;
	if (TK_INT(c))
		return;
	if (c == '@')
		c = exec_buf;
	exec_buf = c;
	buf = reg_get(exec_buf, &lnmode);
	if (buf)
		term_push(buf, strlen(buf));
}

static void vi(void)
{
	int xcol;
	int mark;
	char *ln;
	char *kmap = NULL;
	xtop = 0;
	xrow = 0;
	xoff = 0;
	xcol = vi_off2col(xb, xrow, xoff);
	vi_draw(xcol);
	term_pos(xrow, led_pos(lbuf_get(xb, xrow), xcol));
	while (!xquit) {
		int redraw = 0;
		int nrow = xrow;
		int noff = ren_noeol(lbuf_get(xb, xrow), xoff);
		int otop = xtop;
		int mv, n;
		term_cmd(&n);
		vi_arg2 = 0;
		vi_ybuf = vi_yankbuf();
		vi_arg1 = vi_prefix();
		if (!vi_ybuf)
			vi_ybuf = vi_yankbuf();
		mv = vi_motion(&nrow, &noff);
		if (mv > 0) {
			if (strchr("\'`GHML/?{}[]nN", mv) ||
					(mv == '%' && noff < 0)) {
				lbuf_mark(xb, '\'', xrow, xoff);
				lbuf_mark(xb, '`', xrow, xoff);
			}
			xrow = nrow;
			if (noff < 0 && !strchr("jk", mv))
				noff = lbuf_indents(xb, xrow);
			if (strchr("jk", mv))
				noff = vi_col2off(xb, xrow, xcol);
			xoff = ren_noeol(lbuf_get(xb, xrow), noff);
			if (!strchr("|jk", mv))
				xcol = vi_off2col(xb, xrow, xoff);
			if (mv == '|')
				xcol = vi_pcol;
		} else if (mv == 0) {
			char *cmd;
			int c = vi_read();
			int k;
			if (c <= 0)
				continue;
			lbuf_mark(xb, '*', xrow, xoff);
			switch (c) {
			case TK_CTL('b'):
				if (vi_scrollbackward(MAX(1, vi_arg1) * (xrows - 1)))
					break;
				xoff = lbuf_indents(xb, xrow);
				redraw = 1;
				break;
			case TK_CTL('f'):
				if (vi_scrollforeward(MAX(1, vi_arg1) * (xrows - 1)))
					break;
				xoff = lbuf_indents(xb, xrow);
				redraw = 1;
				break;
			case TK_CTL('e'):
				if (vi_scrollforeward(MAX(1, vi_arg1)))
					break;
				redraw = 1;
				break;
			case TK_CTL('y'):
				if (vi_scrollbackward(MAX(1, vi_arg1)))
					break;
				redraw = 1;
				break;
			case TK_CTL('u'):
				if (xrow == 0)
					break;
				if (vi_arg1)
					vi_scroll = vi_arg1;
				n = vi_scroll ? vi_scroll : xrows / 2;
				xrow = MAX(0, xrow - n);
				if (xtop > 0)
					xtop = MAX(0, xtop - n);
				redraw = 1;
				xoff = lbuf_indents(xb, xrow);
				break;
			case TK_CTL('d'):
				if (xrow == lbuf_len(xb) - 1)
					break;
				if (vi_arg1)
					vi_scroll = vi_arg1;
				n = vi_scroll ? vi_scroll : xrows / 2;
				xrow = MIN(MAX(0, lbuf_len(xb) - 1), xrow + n);
				if (xtop < lbuf_len(xb) - xrows)
					xtop = MIN(lbuf_len(xb) - xrows, xtop + n);
				redraw = 1;
				xoff = lbuf_indents(xb, xrow);
				break;
			case TK_CTL('z'):
				term_pos(xrows, 0);
				term_suspend();
				redraw = 1;
				break;
			case 'u':
				if (!lbuf_undo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					redraw = 1;
				} else {
					snprintf(vi_msg, sizeof(vi_msg), "undo failed\n");
				}
				break;
			case TK_CTL('r'):
				if (!lbuf_redo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					redraw = 1;
				} else {
					snprintf(vi_msg, sizeof(vi_msg), "redo failed\n");
				}
				break;
			case TK_CTL('g'):
				vc_status();
				break;
			case TK_CTL('^'):
				ex_command("e #");
				redraw = 1;
				break;
			case ':':
				ln = vi_prompt(":", &kmap);
				if (ln && ln[0]) {
					ex_command(ln);
					redraw = 1;
				}
				free(ln);
				if (xquit)
					continue;
				break;
			case 'c':
			case 'd':
			case 'y':
			case '!':
			case '>':
			case '<':
				if (!vc_motion(c))
					redraw = 1;
				break;
			case 'i':
			case 'I':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				if (!vc_insert(c))
					redraw = 1;
				break;
			case 'J':
				if (!vc_join())
					redraw = 1;
				break;
			case 'm':
				if ((mark = vi_read()) > 0 && islower(mark))
					lbuf_mark(xb, mark, xrow, xoff);
				break;
			case 'p':
			case 'P':
				if (!vc_put(c))
					redraw = 1;
				break;
			case 'z':
				k = vi_read();
				switch (k) {
				case '\n':
					xtop = vi_arg1 ? vi_arg1 : xrow;
					break;
				case '.':
					n = vi_arg1 ? vi_arg1 : xrow;
					xtop = MAX(0, n - xrows / 2);
					break;
				case '-':
					n = vi_arg1 ? vi_arg1 : xrow;
					xtop = MAX(0, n - xrows + 1);
					break;
				case 'l':
				case 'r':
					xdir = k == 'r' ? -1 : +1;
					break;
				case 'L':
				case 'R':
					xdir = k == 'R' ? -2 : +2;
					break;
				}
				redraw = 1;
				break;
			case 'g':
				k = vi_read();
				if (k == '~' || k == 'u' || k == 'U')
					if (!vc_motion(k))
						redraw = 1;
				break;
			case 'x':
				vi_back(' ');
				if (!vc_motion('d'))
					redraw = 1;
				break;
			case 'X':
				vi_back(TK_CTL('h'));
				if (!vc_motion('d'))
					redraw = 1;
				break;
			case 'C':
				vi_back('$');
				if (!vc_motion('c'))
					redraw = 1;
				break;
			case 'D':
				vi_back('$');
				if (!vc_motion('d'))
					redraw = 1;
				break;
			case 'r':
				if (!vc_replace())
					redraw = 1;
				break;
			case 's':
				vi_back(' ');
				if (!vc_motion('c'))
					redraw = 1;
				break;
			case 'S':
				vi_back('c');
				if (!vc_motion('c'))
					redraw = 1;
				break;
			case 'Y':
				vi_back('y');
				if (!vc_motion('y'))
					redraw = 1;
				break;
			case 'Z':
				k = vi_read();
				if (k == 'Z')
					ex_command("x");
				break;
			case '~':
				vi_back(' ');
				if (!vc_motion('~'))
					redraw = 1;
				break;
			case '.':
				vc_repeat();
				break;
			case '@':
				vc_execute();
				break;
			default:
				continue;
			}
			cmd = term_cmd(&n);
			if (strchr("!<>ACDIJOPRSXYacdioprsxy~", c)) {
				if (n < sizeof(rep_cmd)) {
					memcpy(rep_cmd, cmd, n);
					rep_len = n;
				}
			}
		}
		if (xrow < 0 || xrow >= lbuf_len(xb))
			xrow = lbuf_len(xb) ? lbuf_len(xb) - 1 : 0;
		if (xtop > xrow)
			xtop = xtop - xrows / 2 > xrow ?
					MAX(0, xrow - xrows / 2) : xrow;
		if (xtop + xrows <= xrow)
			xtop = xtop + xrows + xrows / 2 <= xrow ?
					xrow - xrows / 2 : xrow - xrows + 1;
		xoff = ren_noeol(lbuf_get(xb, xrow), xoff);
		if (redraw)
			xcol = vi_off2col(xb, xrow, xoff);
		vi_wait();
		if (redraw || xtop != otop)
			vi_draw(xcol);
		if (vi_msg[0])
			vi_drawmsg();
		term_pos(xrow - xtop, led_pos(lbuf_get(xb, xrow),
				ren_cursor(lbuf_get(xb, xrow), xcol)));
		lbuf_modified(xb);
	}
	term_pos(xrows, 0);
	term_kill();
}

int main(int argc, char *argv[])
{
	int i;
	xvis = 1;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == 's')
			xled = 0;
		if (argv[i][1] == 'e')
			xvis = 0;
		if (argv[i][1] == 'v')
			xvis = 1;
	}
	dir_init();
	syn_init();
	if (xled || xvis)
		term_init();
	ex_init(argv + i);
	if (xvis)
		vi();
	else
		ex();
	if (xled || xvis)
		term_done();
	ex_done();
	reg_done();
	syn_done();
	dir_done();
	return 0;
}
