# minish

Mini shell en C hecha para la evaluacion RA1 de Sistemas Operativos (ULagos).
Tiene que usar fork() y exec(), manejar procesos en primer y segundo plano,
evitar zombies y mostrar reportes de memoria.

## Como compilar y correr

```
gcc -Wall -Wextra -Wpedantic -std=c11 minish.c -o minish
./minish
```

Compila limpio, sin warnings.

## Que hace

- Muestra `minish>` y espera que escribas un comando, usando `getline` (no
  hay limite de largo fijo).
- Comandos internos: `cd`, `help`, `exit`.
- Todo lo demas se busca como programa externo y se corre con `fork()` +
  `execvp()`.
- Si el comando termina en `&` se ejecuta en segundo plano (no se espera,
  puedes seguir escribiendo otros comandos de inmediato).
- Reporta memoria del hijo y del sistema cuando un proceso termina.
- Al hacer `exit` muestra un resumen con las estadisticas de la sesion.

No tiene pipes, redirecciones, ni comillas. El enunciado dice explicitamente
que no es necesario, asi que no se implemento para no salirse del alcance.

## Enum + switch

C no deja hacer switch sobre strings, entonces primero se traduce el nombre
del comando a un enum con strcmp, y despues el switch decide que hacer:

```c
typedef enum { CMD_CD, CMD_HELP, CMD_EXIT, CMD_EXTERNO, CMD_VACIO } TipoComando;

TipoComando identificar_comando(const char *nombre) {
    if (nombre == NULL) return CMD_VACIO;
    if (strcmp(nombre, "cd") == 0) return CMD_CD;
    if (strcmp(nombre, "help") == 0) return CMD_HELP;
    if (strcmp(nombre, "exit") == 0) return CMD_EXIT;
    return CMD_EXTERNO;
}
```

En el main:

```c
switch (tipo) {
    case CMD_VACIO:   break;
    case CMD_CD:      cmd_cd(&cmd); break;
    case CMD_HELP:    cmd_help(); break;
    case CMD_EXIT:    seguir_ejecutando = 0; break;
    case CMD_EXTERNO: ejecutar_comando_externo(&cmd, trabajos, MAX_TRABAJOS, &stats); break;
}
```

Un comando que no existe no tiene caso propio: cae en CMD_EXTERNO y es
execvp() el que falla y avisa el error (asi se detecta "comando inexistente"
sin duplicar logica en otro lado).

## fork, execvp, waitpid

```c
pid_t pid = fork();
if (pid < 0) { perror("minish: fork"); return; }

if (pid == 0) {
    execvp(cmd->argv[0], cmd->argv);
    fprintf(stderr, "minish: no se pudo ejecutar '%s': %s\n", cmd->argv[0], strerror(errno));
    _exit(127);
}
```

fork() crea una copia del proceso. El que recibe 0 es el hijo, el que recibe
el pid del hijo es el padre. execvp() reemplaza la imagen del hijo por el
programa pedido (busca en PATH). Si execvp() retorna, fallo, por eso el hijo
tiene que terminar ahi mismo con _exit() y no puede seguir el loop de la
shell como si nada.

waitpid() se usa siempre con WNOHANG, nunca bloqueante puro, porque la misma
funcion (revisar_trabajo) sirve tanto para primer como para segundo plano
(ver siguiente punto).

## Primer plano, segundo plano y los zombies

Hay una sola funcion, `revisar_trabajo()`, que hace `waitpid(pid, &status,
WNOHANG)`:

- si el proceso sigue vivo, aprovecha de leer su memoria en
  `/proc/pid/status` y vuelve.
- si ya termino, actualiza las estadisticas, muestra el reporte y marca el
  slot como libre.

Para primer plano (`esperar_primer_plano`) se llama esta funcion en un loop
con `nanosleep(20ms)` hasta que el proceso termine (la shell no puede seguir
hasta que termine, pero asi igual se van tomando muestras de memoria).

Para segundo plano no hay loop: `recolectar_procesos()` revisa todos los
trabajos activos una vez por vuelta del bucle principal, justo antes de
pedir el siguiente comando. No bloquea nunca.

Por que no se usa SIGCHLD: como se revisa todo constantemente (cada vez que
se va a pedir un comando nuevo, y tambien mientras se espera un proceso en
primer plano), el
unico momento en que un hijo de background podria quedar sin recolectar es
mientras el usuario esta escribiendo el siguiente comando, y ahi se recolecta
apenas aprieta enter. No es un zombie permanente, solo un instante, y asi se
evita meterse con manejadores de señales (que son mas delicados: no se puede
hacer printf ni tocar estructuras compartidas ahi dentro sin cuidado).

Cada trabajo tiene un estado (libre/activo) para no contar dos veces el mismo
proceso: en cuanto se recolecta se marca libre y no se vuelve a tocar.

## Memoria

Para la memoria disponible del sistema se lee `/proc/meminfo`, campo
`MemAvailable`. Es exacta en el momento en que se lee.

Para la memoria del hijo el problema es que waitpid() (a diferencia de
wait4()) no entrega esa informacion, y una vez que se recolecta el proceso
su carpeta `/proc/pid/status` ya no existe. Por eso, mientras el proceso
sigue vivo (cuando `revisar_trabajo` recibe 0 de waitpid con WNOHANG), se
lee `/proc/pid/status` (campo VmRSS) y se guarda esa ultima muestra. Cuando
el proceso termina, se reporta esa muestra.

Esto quiere decir que **no es necesariamente el maximo de memoria usada**,
es la ultima lectura antes de que terminara (cada 20ms en primer plano, o
cada vez que se revisa en segundo plano). Para procesos muy rapidos puede que
no haya ninguna muestra (se reporta como no disponible, no se inventa un
numero).

## Resumen final

Contadores, cada uno cuenta una sola vez por proceso:

- iniciados_ok: cuantos fork() de comandos externos salieron bien.
- terminados: cuantos de esos se llegaron a recolectar con waitpid.
- exitosos / con_error / con_senal: de los terminados, en cual de los tres
  termino cada uno (son excluyentes entre si).
- plano_iniciados: cuantos de los iniciados fueron con `&`.
- activos al salir: los que quedaron en estado activo despues del ultimo
  barrido no bloqueante.

Si al hacer exit quedan trabajos de background activos, minish no los espera
ni los mata: se informa cuantos quedan y se les deja seguir corriendo por su
cuenta (quedan huerfanos, los adopta init).

## Pruebas que hice

Compile con las flags del enunciado (0 warnings) y probe a mano, con
`./minish` corriendo normal en la terminal:

- `pwd`, `ls -l /tmp`, `help` -> funcionan bien.
- `cd /tmp` y despues `pwd` -> muestra /tmp. `cd` solo -> vuelve a $HOME.
- comando que no existe -> tira el error de execvp, codigo 127, sigue la shell.
- `sleep 2` normal -> la shell espera, no pide el siguiente comando hasta que termina.
- `sleep 5 &` -> se puede escribir otro comando de inmediato, avisa cuando termina.
- dos `sleep &` al mismo tiempo -> los dos avisan su inicio y su fin por
  separado, ninguno se cuenta dos veces.
- reporte de memoria se ve en los dos casos (primer y segundo plano).
- `exit` con un `sleep 30 &` corriendo -> el resumen avisa que queda 1
  trabajo activo y que sigue corriendo por su cuenta (lo revise con pgrep
  en otra terminal, efectivamente seguia vivo despues de que minish cerro).
- probe una linea de ~5000 caracteres (un echo gigante) y no se corta, por
  el getline.
- probe matar un `sleep 5` en primer plano con `kill -9` desde otra terminal
  mientras minish lo esperaba: el reporte mostro "terminado por senal 9" y
  el resumen lo conto en con_senal, no en con_error.

Todo lo anterior lo corri de verdad, no es una lista de lo que "deberia"
pasar.
