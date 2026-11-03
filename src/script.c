/* Terminator — script.c
 * The script interpreter (Design section 9): structure validation,
 *  * [ ] tests, and line-by-line execution through the shell engine.
 */
#include "common.h"


/* Minimal in-process script interpreter. Supports nested if/elif/else/fi,
   assignments, positional parameters, source, pipelines, redirection and exit. */
typedef struct { int parent_active, branch_taken, active, saw_else; } IfFrame;

static int script_test(char *expr, Tab *t) {
    while (*expr == ' ' || *expr == '\t') expr++;
    size_t n = strlen(expr);
    /* Tolerate the usual "if [ ... ]; then" tail. */
    for (;;) {
        while (n && (expr[n-1] == ' ' || expr[n-1] == '\t')) expr[--n] = 0;
        if (n && expr[n-1] == ';') { expr[--n] = 0; continue; }
        if (n >= 4 && strcmp(expr + n - 4, "then") == 0 &&
            (n == 4 || expr[n-5] == ' ' || expr[n-5] == '\t' || expr[n-5] == ';')) { n -= 4; expr[n] = 0; continue; }
        break;
    }
    if (*expr == '[') expr++;
    n = strlen(expr); if (n && expr[n-1] == ']') expr[--n] = 0;
    while (n && (expr[n-1] == ' ' || expr[n-1] == '\t')) expr[--n] = 0;
    char *v[8] = {0}; int nv = 0; char *save = NULL;
    for (char *q = strtok_r(expr, " \t", &save); q && nv < 8; q = strtok_r(NULL, " \t", &save)) {
        /* strip one layer of surrounding quotes */
        size_t ql = strlen(q);
        if (ql >= 2 && ((q[0] == '"' && q[ql-1] == '"') || (q[0] == '\'' && q[ql-1] == '\''))) { q[ql-1] = 0; q++; }
        v[nv++] = q;
    }
    if (nv == 0) return 0;
    int neg = 0; char **w = v;
    if (!strcmp(w[0], "!")) { neg = 1; w++; nv--; if (nv == 0) return 0; }
    int r = 0;
    if (nv == 1) {
        r = w[0][0] != 0;
    } else if (nv == 2) {
        const char *op = w[0], *a = w[1];
        struct stat st; char resolved[PATH_MAX];
        const char *rp = a;
        /* Paths in [ ] tests resolve against the tab's cwd, like the
           commands the tab runs, not the process's launch directory. */
        if (a[0] && a[0] != '/') { join_path(resolved, sizeof(resolved), t->cwd, a); rp = resolved; }
        if (!strcmp(op, "-f")) r = stat(rp, &st) == 0 && S_ISREG(st.st_mode);
        else if (!strcmp(op, "-d")) r = stat(rp, &st) == 0 && S_ISDIR(st.st_mode);
        else if (!strcmp(op, "-e")) r = access(rp, F_OK) == 0;
        else if (!strcmp(op, "-n")) r = a[0] != 0;
        else if (!strcmp(op, "-z")) r = a[0] == 0;
        else r = 0;
    } else {
        const char *a = w[0], *op = w[1], *b = w[2];
        if (!strcmp(op, "=") || !strcmp(op, "==")) r = strcmp(a, b) == 0;
        else if (!strcmp(op, "!=")) r = strcmp(a, b) != 0;
        else {
            char *ea = NULL, *eb = NULL;
            long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
            if (ea && *ea == 0 && eb && *eb == 0) {
                if (!strcmp(op, "-eq")) r = x == y;
                else if (!strcmp(op, "-ne")) r = x != y;
                else if (!strcmp(op, "-gt")) r = x > y;
                else if (!strcmp(op, "-ge")) r = x >= y;
                else if (!strcmp(op, "-lt")) r = x < y;
                else if (!strcmp(op, "-le")) r = x <= y;
            }
        }
    }
    return neg ? !r : r;
}

/* Cut a script line at an unquoted '#', preserving '#' inside quotes.
   Also used for typed lines: a '#' at the start of a word is a comment. */
void strip_comment(char *p) {
    int sq = 0, dq = 0;
    for (char *q = p; *q; q++) {
        if (*q == '\'' && !dq) sq = !sq;
        else if (*q == '"' && !sq) dq = !dq;
        else if (*q == '#' && !sq && !dq && (q == p || q[-1] == ' ' || q[-1] == '\t')) { *q = 0; break; }
    }
}
static int validate_script_structure(FILE *f, Tab *t) {
    char *line = NULL; size_t cap = 0; ssize_t nr; int nesting = 0, bad = 0;
    unsigned char saw_else[32] = {0};
    char hd_delim[64]; int hd_active = 0, hd_quoted = 0;
    while ((nr = getline(&line, &cap, f)) >= 0) {
        while (nr > 0 && (line[nr-1] == '\n' || line[nr-1] == '\r')) line[--nr] = 0;
        /* Heredoc bodies are data: skip them whole, so a body line that
           reads "if" or "fi" cannot disturb the nesting check. The
           delimiter line must match exactly, as in execution. */
        if (hd_active) {
            if (!strcmp(line, hd_delim)) hd_active = 0;
            continue;
        }
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        strip_comment(p);
        size_t n = strlen(p); while (n && (p[n-1] == ' ' || p[n-1] == '\t')) p[--n] = 0;
        if (heredoc_find(p, hd_delim, sizeof(hd_delim), &hd_quoted) > 0) { hd_active = 1; continue; }
        if (!strncmp(p, "if ", 3)) {
            if (nesting >= 32) { bad = 1; break; }
            saw_else[nesting++] = 0;
        } else if (!strncmp(p, "elif ", 5)) {
            if (nesting == 0 || saw_else[nesting-1]) { bad = 1; break; }
        } else if (!strcmp(p, "else")) {
            if (nesting == 0 || saw_else[nesting-1]) { bad = 1; break; }
            saw_else[nesting-1] = 1;
        } else if (!strcmp(p, "fi")) {
            if (nesting == 0) { bad = 1; break; }
            nesting--;
        }
    }
    if (nesting != 0) bad = 1;
    free(line); clearerr(f); rewind(f);
    if (bad) append_text(t, "Syntax error in script: unmatched or misplaced if/elif/else/fi; no commands executed.\n");
    return bad ? 2 : 0;
}

int run_script_file(Tab *t, const char *path, int argc, char **argv, int depth) {
    if(depth>8){append_text(t,"source: maximum script nesting exceeded\n");return 2;}
    char resolved[PATH_MAX];
    int pathn = path[0]=='/' ? snprintf(resolved,sizeof(resolved),"%s",path) : snprintf(resolved,sizeof(resolved),"%s/%s",t->cwd,path);
    if(pathn<0 || pathn>=(int)sizeof(resolved)){append_text(t,"source: path too long\n");return 2;}
    FILE *f=fopen(resolved,"r"); if(!f){print_error(t,path);return 1;}
    int validation = validate_script_structure(f, t);
    if (validation != 0) { fclose(f); return validation; }
    char argc_text[24]; snprintf(argc_text,sizeof(argc_text),"%d",argc); setenv("TERMINATOR_ARGC",argc_text,1);
    setenv("TERMINATOR_ARG_0", resolved, 1); /* $0: the script's own path */
    for(int ai=1;ai<=9;ai++){char key[32];snprintf(key,sizeof(key),"TERMINATOR_ARG_%d",ai);if(ai<=argc&&argv&&argv[ai-1])setenv(key,argv[ai-1],1);else unsetenv(key);}
    IfFrame stack[32]; int depth_if=0, status=0; char *line=NULL; size_t cap=0; ssize_t nr;
    while((nr=getline(&line,&cap,f))>=0){
        while(nr>0&&(line[nr-1]=='\n'||line[nr-1]=='\r'))line[--nr]=0;
        char *p=line; while(*p==' '||*p=='\t')p++;
        if(!*p||*p=='#')continue;
        /* Strip an unquoted trailing comment, preserving # inside quotes. */
        strip_comment(p);
        size_t plen=strlen(p);while(plen&&(p[plen-1]==' '||p[plen-1]=='\t'))p[--plen]=0;if(!plen)continue;
        int active=(depth_if==0)?1:stack[depth_if-1].active;
        if(!strncmp(p,"if ",3)){
            if(depth_if>=32){append_text(t,"Syntax error: if nesting too deep\n");status=2;break;}
            int parent=active, ok=0;
            if(parent){char expr[INPUT_CAP];snprintf(expr,sizeof(expr),"%s",p+3);if(expand_variables(t,expr,sizeof(expr))<0||expand_arithmetic(expr,sizeof(expr))<0){status=2;break;}ok=script_test(expr,t);}
            stack[depth_if++]=(IfFrame){parent,ok,parent&&ok,0}; continue;
        }
        if(!strncmp(p,"elif ",5)){
            if(!depth_if||stack[depth_if-1].saw_else){append_text(t,"Syntax error: unexpected elif\n");status=2;break;}
            IfFrame *fr=&stack[depth_if-1]; int ok=0;
            if(fr->parent_active&&!fr->branch_taken){char expr[INPUT_CAP];snprintf(expr,sizeof(expr),"%s",p+5);if(expand_variables(t,expr,sizeof(expr))<0||expand_arithmetic(expr,sizeof(expr))<0){status=2;break;}ok=script_test(expr,t);}
            fr->active=fr->parent_active&&!fr->branch_taken&&ok; if(ok)fr->branch_taken=1; continue;
        }
        if(!strcmp(p,"else")){
            if(!depth_if||stack[depth_if-1].saw_else){append_text(t,"Syntax error: unexpected else\n");status=2;break;}
            IfFrame *fr=&stack[depth_if-1];fr->saw_else=1;fr->active=fr->parent_active&&!fr->branch_taken;fr->branch_taken=1;continue;
        }
        if(!strcmp(p,"fi")){
            if(!depth_if){append_text(t,"Syntax error: unexpected fi\n");status=2;break;}depth_if--;continue;
        }
        /* Heredoc: consume the following lines as the body (raw, with no
           comment stripping) up to the delimiter line, even in an
           inactive branch where the whole command is skipped. The
           assembled text goes through the shell engine, which feeds the
           body to the command as stdin. */
        {
            char hd_delim[64]; int hd_quoted = 0;
            int hf = heredoc_find(p, hd_delim, sizeof(hd_delim), &hd_quoted);
            if (hf < 0) { append_text(t, "Syntax error: '<<' needs a delimiter word, e.g. <<EOF.\n"); status = 2; continue; }
            if (hf > 0) {
                char assembled[INPUT_CAP]; size_t alen = 0;
                int found_delim = 0, too_long = 0;
                size_t plen2 = strlen(p);
                if (plen2 + 1 < sizeof(assembled)) { memcpy(assembled, p, plen2 + 1); alen = plen2; }
                else too_long = 1;
                char *sline = NULL; size_t scap = 0; ssize_t snr;
                while ((snr = getline(&sline, &scap, f)) >= 0) {
                    while (snr > 0 && (sline[snr - 1] == '\n' || sline[snr - 1] == '\r')) sline[--snr] = 0;
                    if (!strcmp(sline, hd_delim)) { found_delim = 1; break; }
                    if (!too_long) {
                        if (alen + (size_t)snr + 2 < sizeof(assembled)) {
                            assembled[alen++] = '\n';
                            memcpy(assembled + alen, sline, (size_t)snr); alen += (size_t)snr;
                            assembled[alen] = '\0';
                        } else too_long = 1;
                    }
                }
                free(sline);
                if (!found_delim) { append_text(t, "Syntax error: heredoc delimiter not found in script.\n"); status = 2; break; }
                if (too_long) { append_text(t, "Syntax error: heredoc body is too long.\n"); status = 2; continue; }
                /* The engine's heredoc extraction needs the delimiter
                   line itself, so append it after the body lines. */
                {
                    size_t dl = strlen(hd_delim);
                    if (alen + dl + 2 < sizeof(assembled)) {
                        assembled[alen++] = '\n';
                        memcpy(assembled + alen, hd_delim, dl); alen += dl;
                        assembled[alen] = '\0';
                    } else { append_text(t, "Syntax error: heredoc body is too long.\n"); status = 2; continue; }
                }
                if (!active) continue;
                status = execute_command_sync(t, assembled);
                continue;
            }
        }
        if(!active)continue;
        {   /* NAME=value assignment: expand once, here, and store it. */
            char *eq = strchr(p, '=');
            int is_assign = 0;
            if (eq && eq != p) {
                is_assign = 1;
                for (char *q = p; q < eq; q++) if (*q == ' ' || *q == '\t') { is_assign = 0; break; }
            }
            if (is_assign) {
                /* Expand in a fixed-size buffer: p is getline's buffer,
                   which may be smaller than INPUT_CAP. */
                char ebuf[INPUT_CAP]; snprintf(ebuf, sizeof(ebuf), "%s", p);
                if (expand_variables(t, ebuf, sizeof(ebuf)) < 0 || expand_arithmetic(ebuf, sizeof(ebuf)) < 0) { append_text(t, "script: expansion too long\n"); status = 2; break; }
                eq = strchr(ebuf, '=');
                *eq = 0;
                char *val = eq + 1; size_t vl = strlen(val);
                if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') || (val[0] == '\'' && val[vl - 1] == '\''))) { val[vl - 1] = 0; val++; }
                if (setenv(ebuf, val, 1) < 0) { print_error(t, "setenv"); status = 1; }
                continue;
            }
        }
        if(!strncmp(p,"exit",4)&&(p[4]==' '||p[4]==0)){int n=status;if(p[4])sscanf(p+4,"%d",&n);status=((n%256)+256)%256;break;}
        if(!strncmp(p,"source ",7)||!strncmp(p,". ",2)){
            char *sub=p+(!strncmp(p,"source ",7)?7:2);while(*sub==' ')sub++;char *av[9];int ac=0;char *save=NULL;for(char *q=strtok_r(sub," \t",&save);q&&ac<9;q=strtok_r(NULL," \t",&save))av[ac++]=q;
            if(!ac){append_text(t,"source: missing filename\n");status=2;}else status=run_script_file(t,av[0],ac-1,ac>1?av+1:NULL,depth+1);continue;
        }
        if(!strncmp(p,"bash ",5)){char *sub=p+5;while(*sub==' ')sub++;char *av[9];int ac=0;char *save=NULL;for(char *q=strtok_r(sub," \t",&save);q&&ac<9;q=strtok_r(NULL," \t",&save))av[ac++]=q;if(ac)status=run_script_file(t,av[0],ac-1,ac>1?av+1:NULL,depth+1);else status=2;continue;}
        /* Every other line goes through the full shell engine (built-ins,
           pipes, redirection, substitution, jobs) and is waited for. */
        status = execute_command_sync(t, p);
    }
    if(depth_if){append_text(t,"Syntax error: missing fi in script\n");status=2;}
    free(line);fclose(f);return status;
}
