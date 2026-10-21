/*
 * minish.c
 * Mini shell para la evaluacion de Sistemas Operativos (ULagos).
 * Implementa fork/exec, comandos internos, procesos en background
 * y reportes de memoria usando /proc.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#define MAX_ARGS 64
#define MAX_TRABAJOS 128
#define SAMPLE_INTERVAL_NS 20000000L /* 20 ms entre muestras de memoria */

/* como en C no se puede hacer switch sobre strings, se usa este enum
   para poder identificar el comando con strcmp() y despues decidir
   que hacer con un switch normal */
typedef enum {
    CMD_CD,
    CMD_HELP,
    CMD_EXIT,
    CMD_EXTERNO,
    CMD_VACIO
} TipoComando;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    int segundo_plano; /* 1 si el comando venia con "&" al final */
} Comando;

typedef enum { TRABAJO_LIBRE, TRABAJO_ACTIVO } EstadoTrabajo;

/* un slot por cada proceso que estamos siguiendo (en fg o bg) */
typedef struct {
    pid_t pid;
    int segundo_plano;
    long last_rss_kb;   /* ultima memoria que logramos leer, -1 si ninguna */
    EstadoTrabajo estado;
} Trabajo;

typedef struct {
    int iniciados_ok;
    int terminados;
    int exitosos;
    int con_error;
    int con_senal;
    int plano_iniciados;
} Estadisticas;

static void mostrar_prompt(void);
static char *leer_comando(void);
static void parsear_comando(char *linea, Comando *cmd);
static TipoComando identificar_comando(const char *nombre);

static void cmd_cd(Comando *cmd);
static void cmd_help(void);

static long leer_mem_disponible_kb(void);
static long muestrear_rss_kb(pid_t pid);
static void reportar_memoria(pid_t pid, int segundo_plano, int fue_senal,
                              int codigo, long rss_kb);

static int buscar_slot_libre(Trabajo trabajos[], int n);
static int revisar_trabajo(Trabajo *t, Estadisticas *stats);
static void esperar_primer_plano(Trabajo *t, Estadisticas *stats);
static void recolectar_procesos(Trabajo trabajos[], int n, Estadisticas *stats);

static void ejecutar_comando_externo(Comando *cmd, Trabajo trabajos[],
                                      int n, Estadisticas *stats);

static void mostrar_resumen_final(Estadisticas *stats, Trabajo trabajos[], int n);

int main(void)
{
    Trabajo trabajos[MAX_TRABAJOS];
    for (int i = 0; i < MAX_TRABAJOS; i++) {
        trabajos[i].estado = TRABAJO_LIBRE;
    }

    Estadisticas stats = {0, 0, 0, 0, 0, 0};
    int seguir_ejecutando = 1;

    while (seguir_ejecutando) {
        /* antes de pedir el siguiente comando revisamos si algun
           trabajo en background ya termino, asi no quedan zombies */
        recolectar_procesos(trabajos, MAX_TRABAJOS, &stats);

        mostrar_prompt();
        char *linea = leer_comando();
        if (linea == NULL) {
            printf("\n");
            break; /* ctrl+d o error de lectura, se sale igual que con exit */
        }

        Comando cmd;
        parsear_comando(linea, &cmd);
        TipoComando tipo = identificar_comando(cmd.argv[0]);

        switch (tipo) {
            case CMD_VACIO:
                /* el usuario solo apreto enter, no hacemos nada */
                break;

            case CMD_CD:
                cmd_cd(&cmd);
                break;

            case CMD_HELP:
                cmd_help();
                break;

            case CMD_EXIT:
                seguir_ejecutando = 0;
                break;

            case CMD_EXTERNO:
                ejecutar_comando_externo(&cmd, trabajos, MAX_TRABAJOS, &stats);
                break;
        }

        free(linea);
    }

    /* ultimo barrido antes de salir, pero sin bloquear. si queda algo
       vivo en background se informa en el resumen y listo */
    recolectar_procesos(trabajos, MAX_TRABAJOS, &stats);
    mostrar_resumen_final(&stats, trabajos, MAX_TRABAJOS);

    return 0;
}

static void mostrar_prompt(void)
{
    char *directorio = getcwd(NULL, 0);

    if (directorio != NULL) {
        printf("minish:%s> ", directorio);
        free(directorio);
    } else {
        printf("minish> ");
    }

    fflush(stdout);
}

static char *leer_comando(void)
{
    char *linea = NULL;
    size_t cap = 0;

    /* getline se encarga de hacer crecer el buffer solo, asi que
       no hay problema con lineas largas */
    ssize_t len = getline(&linea, &cap, stdin);
    if (len == -1) {
        if (ferror(stdin)) {
            perror("minish: error al leer la entrada");
        }
        free(linea);
        return NULL;
    }

    if (len > 0 && linea[len - 1] == '\n') {
        linea[len - 1] = '\0';
    }

    return linea;
}

static void parsear_comando(char *linea, Comando *cmd)
{
    cmd->argc = 0;
    cmd->segundo_plano = 0;

    char *token = strtok(linea, " \t");
    while (token != NULL && cmd->argc < MAX_ARGS - 1) {
        cmd->argv[cmd->argc++] = token;
        token = strtok(NULL, " \t");
    }
    cmd->argv[cmd->argc] = NULL;

    /* si el ultimo token es "&" lo sacamos de los argumentos y
       marcamos el comando como background */
    if (cmd->argc > 0 && strcmp(cmd->argv[cmd->argc - 1], "&") == 0) {
        cmd->segundo_plano = 1;
        cmd->argc--;
        cmd->argv[cmd->argc] = NULL;
    }
}

static TipoComando identificar_comando(const char *nombre)
{
    if (nombre == NULL) return CMD_VACIO;
    if (strcmp(nombre, "cd") == 0) return CMD_CD;
    if (strcmp(nombre, "help") == 0) return CMD_HELP;
    if (strcmp(nombre, "exit") == 0) return CMD_EXIT;
    return CMD_EXTERNO; /* si no es ninguno de los internos, se intenta ejecutar */
}

/* cd tiene que correr en el proceso de la shell, no en un hijo,
   porque chdir() solo cambia el directorio del proceso que lo llama */
static void cmd_cd(Comando *cmd)
{
    char *destino = cmd->argv[1];

    if (destino == NULL) {
        destino = getenv("HOME");
        if (destino == NULL) {
            fprintf(stderr, "minish: cd: la variable HOME no esta definida\n");
            return;
        }
    }

    if (chdir(destino) != 0) {
        fprintf(stderr, "minish: cd: %s: %s\n", destino, strerror(errno));
    }
}

static void cmd_help(void)
{
    printf(
        "minish - mini shell hecha para la evaluacion de Sistemas Operativos\n"
        "Comandos internos:\n"
        "  cd [directorio]   cambia el directorio de trabajo (si no se da, usa $HOME)\n"
        "  help              muestra esta ayuda\n"
        "  exit              termina minish y muestra el resumen final\n"
        "Cualquier otra palabra se busca como programa externo y se ejecuta con\n"
        "fork()+execvp(), por ejemplo:\n"
        "  pwd\n"
        "  ls -l /tmp\n"
        "Para que un comando corra en segundo plano se le agrega un '&' al final,\n"
        "separado por espacio:\n"
        "  sleep 5 &\n"
        "con eso puedes seguir escribiendo otros comandos de inmediato,\n"
        "y minish avisa cuando ese proceso termine.\n"
    );
}

static long leer_mem_disponible_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) return -1;

    char buf[256];
    long valor = -1;
    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (strncmp(buf, "MemAvailable:", 13) == 0) {
            sscanf(buf, "MemAvailable: %ld kB", &valor);
            break;
        }
    }
    fclose(f);
    return valor;
}

/* lee VmRSS de /proc/[pid]/status mientras el proceso todavia existe.
   una vez que waitpid() recolecta al hijo esta carpeta desaparece,
   por eso hay que muestrear antes, no despues */
static long muestrear_rss_kb(pid_t pid)
{
    char ruta[64];
    snprintf(ruta, sizeof(ruta), "/proc/%d/status", (int)pid);

    FILE *f = fopen(ruta, "r");
    if (f == NULL) return -1;

    char buf[256];
    long rss = -1;
    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (strncmp(buf, "VmRSS:", 6) == 0) {
            sscanf(buf, "VmRSS: %ld kB", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

static void reportar_memoria(pid_t pid, int segundo_plano, int fue_senal,
                              int codigo, long rss_kb)
{
    long mem_disp = leer_mem_disponible_kb();

    printf("Proceso %d terminado\n", (int)pid);
    printf("  tipo de ejecucion: %s\n", segundo_plano ? "segundo plano" : "primer plano");

    if (fue_senal) {
        printf("  termino por senal %d (%s)\n", codigo, strsignal(codigo));
    } else {
        printf("  termino normal, codigo de salida %d\n", codigo);
    }

    /* esto es la ultima muestra que se pudo tomar antes de que terminara,
       no necesariamente el maximo uso de memoria del proceso */
    if (rss_kb >= 0) {
        printf("  memoria que uso el hijo (ultima lectura de VmRSS): %ld KB\n", rss_kb);
    } else {
        printf("  memoria del hijo: no se pudo medir a tiempo\n");
    }

    if (mem_disp >= 0) {
        printf("  memoria disponible en el sistema ahora: %ld KB\n", mem_disp);
    } else {
        printf("  memoria disponible en el sistema: no disponible\n");
    }
    printf("\n");
}

static int buscar_slot_libre(Trabajo trabajos[], int n)
{
    for (int i = 0; i < n; i++) {
        if (trabajos[i].estado == TRABAJO_LIBRE) return i;
    }
    return -1;
}

/* chequea un trabajo sin bloquear (waitpid con WNOHANG).
   si ya termino, actualiza las estadisticas y muestra el reporte,
   y devuelve 1. si sigue vivo, aprovecha de sacar una muestra de
   memoria y devuelve 0. */
static int revisar_trabajo(Trabajo *t, Estadisticas *stats)
{
    int status;
    pid_t r = waitpid(t->pid, &status, WNOHANG);

    if (r == 0) {
        long rss = muestrear_rss_kb(t->pid);
        if (rss >= 0) t->last_rss_kb = rss;
        return 0;
    }

    if (r == -1) {
        if (errno == EINTR) return 0; /* se intenta de nuevo despues */
        t->estado = TRABAJO_LIBRE;
        return 1;
    }

    /* si llegamos aca, r == t->pid, el proceso termino */
    stats->terminados++;
    int fue_senal = WIFSIGNALED(status);
    int codigo = fue_senal ? WTERMSIG(status) : WEXITSTATUS(status);

    if (fue_senal) {
        stats->con_senal++;
    } else if (codigo == 0) {
        stats->exitosos++;
    } else {
        stats->con_error++;
    }

    if (t->segundo_plano) {
        printf("[minish] trabajo en segundo plano finalizado (PID %d)\n", (int)t->pid);
    }

    reportar_memoria(t->pid, t->segundo_plano, fue_senal, codigo, t->last_rss_kb);

    t->estado = TRABAJO_LIBRE;
    return 1;
}

/* para un comando en primer plano la shell tiene que esperar antes
   de poder pedir el siguiente comando. en vez de usar waitpid bloqueante,
   se hace un loop corto con nanosleep para poder ir muestreando la
   memoria mientras el proceso sigue corriendo */
static void esperar_primer_plano(Trabajo *t, Estadisticas *stats)
{
    int terminado;
    do {
        terminado = revisar_trabajo(t, stats);
        if (!terminado) {
            struct timespec ts = {0, SAMPLE_INTERVAL_NS};
            nanosleep(&ts, NULL);
        }
    } while (!terminado);
}

/* recorre todos los trabajos activos sin bloquear. se llama una vez
   por vuelta del bucle principal, antes de pedir el siguiente comando.
   esto reemplaza a usar un manejador de SIGCHLD: como se revisa
   constantemente, un hijo de background nunca queda mucho tiempo
   sin recolectar (en el peor caso, mientras el usuario escribe) */
static void recolectar_procesos(Trabajo trabajos[], int n, Estadisticas *stats)
{
    for (int i = 0; i < n; i++) {
        if (trabajos[i].estado == TRABAJO_ACTIVO) {
            revisar_trabajo(&trabajos[i], stats);
        }
    }
}

static void ejecutar_comando_externo(Comando *cmd, Trabajo trabajos[],
                                      int n, Estadisticas *stats)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("minish: fork");
        return;
    }

    if (pid == 0) {
        /* esto lo ejecuta el hijo */
        execvp(cmd->argv[0], cmd->argv);
        /* si execvp vuelve es porque fallo, no puede seguir como si nada */
        fprintf(stderr, "minish: no se pudo ejecutar '%s': %s\n",
                cmd->argv[0], strerror(errno));
        _exit(127);
    }

    /* esto lo ejecuta el padre, que sigue siendo la shell */
    stats->iniciados_ok++;

    int slot = buscar_slot_libre(trabajos, n);
    if (slot < 0) {
        /* no deberia pasar con MAX_TRABAJOS=128, pero por si acaso */
        fprintf(stderr, "minish: hay demasiados trabajos activos\n");
        int status;
        waitpid(pid, &status, 0);
        stats->terminados++;
        if (WIFSIGNALED(status)) stats->con_senal++;
        else if (WEXITSTATUS(status) == 0) stats->exitosos++;
        else stats->con_error++;
        return;
    }

    trabajos[slot].pid = pid;
    trabajos[slot].segundo_plano = cmd->segundo_plano;
    trabajos[slot].last_rss_kb = -1;
    trabajos[slot].estado = TRABAJO_ACTIVO;

    if (cmd->segundo_plano) {
        stats->plano_iniciados++;
        printf("[minish] trabajo iniciado en segundo plano, PID %d\n", (int)pid);
    } else {
        esperar_primer_plano(&trabajos[slot], stats);
    }
}

static void mostrar_resumen_final(Estadisticas *stats, Trabajo trabajos[], int n)
{
    int pendientes = 0;
    for (int i = 0; i < n; i++) {
        if (trabajos[i].estado == TRABAJO_ACTIVO) pendientes++;
    }

    long mem_disp = leer_mem_disponible_kb();

    printf("\nResumen de la sesion:\n");
    printf("  procesos externos que se lograron iniciar: %d\n", stats->iniciados_ok);
    printf("  procesos que terminaron y se recolectaron: %d\n", stats->terminados);
    printf("    con salida exitosa (codigo 0): %d\n", stats->exitosos);
    printf("    con error (codigo != 0): %d\n", stats->con_error);
    printf("    terminados por senal: %d\n", stats->con_senal);
    printf("  trabajos que se iniciaron en segundo plano: %d\n", stats->plano_iniciados);
    printf("  trabajos en segundo plano que quedaron activos: %d\n", pendientes);

    if (pendientes > 0) {
        /* no se espera ni se matan, quedan corriendo solos (huerfanos) */
        printf("  (esos procesos no son esperados ni terminados por minish,\n"
               "   siguen corriendo por su cuenta y quedan adoptados por init)\n");
    }

    if (mem_disp >= 0) {
        printf("  memoria disponible en el sistema al finalizar: %ld KB\n", mem_disp);
    } else {
        printf("  memoria disponible en el sistema al finalizar: no disponible\n");
    }
}
