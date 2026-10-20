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

## Guion para la demo (10 min aprox)

1. Explicar brevemente el objetivo (fork/exec, fg/bg, sin zombies, memoria).
2. `pwd`, `ls -l /tmp` -> mostrar que recibe argumentos.
3. `help`, `cd /tmp`, `cd` -> comandos internos.
4. Mostrar el enum y el switch en el codigo, explicar por que no se puede
   hacer switch directo sobre el string del comando.
5. Mostrar `ejecutar_comando_externo`: fork + execvp, probar un comando que
   no existe para que se vea el manejo de error.
6. `sleep 3` (fg, se espera) vs `sleep 5 &` (bg, se puede seguir usando la
   shell al tiro). Lanzar dos `&` seguidos para que se vea que corren a la vez.
7. Explicar `revisar_trabajo` + WNOHANG y por que no hace falta SIGCHLD.
8. Mostrar un reporte de memoria y aclarar que es la ultima muestra, no el
   pico.
9. `exit` con algo corriendo en background, mostrar el resumen y explicar
   que pasa con lo que queda activo.
10. Cierre y preguntas.

## Preguntas que me pueden hacer

**Que hace fork()?** Crea un proceso nuevo que es una copia del que lo
llama. Quedan corriendo como dos procesos separados, cada uno con su propia
memoria (no comparten nada despues del fork).

**Diferencia entre el padre y el hijo?** El valor que devuelve fork(): al
hijo le llega 0, al padre le llega el pid del hijo. En minish, el padre
sigue siendo la shell (espera o sigue al tiro si es background) y el hijo
hace execvp.

**Que hace execvp()?** Reemplaza el programa que esta corriendo el proceso
actual por otro, buscandolo en el PATH. Si funciona, no vuelve (el proceso
ahora "es" el otro programa).

**Por que waitpid()?** Para recoger el estado final de un hijo y que no
quede como zombie. Con WNOHANG se puede preguntar "ya terminaste?" sin
bloquear la shell.

**Que es un proceso zombie?** Un proceso que ya termino pero su entrada en
la tabla de procesos sigue ahi porque nadie hizo wait/waitpid por el.

**Como funciona el segundo plano?** Se detecta el "&" al final al parsear el
comando. Se hace fork() igual que siempre, pero el padre no espera: anota el
pid en la lista de trabajos y vuelve al prompt. Despues se va revisando con
WNOHANG.

**Para que sirve WNOHANG?** Para que waitpid() no bloquee si el hijo todavia
no termino, simplemente devuelve 0 al tiro.

**Por que cd tiene que ser interno?** Porque chdir() cambia el directorio
del proceso que lo ejecuta. Si se hiciera con fork+exec, el cambio se
perderia apenas termina ese proceso hijo; la shell nunca cambiaria de
directorio.

**Por que switch con enum?** Porque C no permite comparar strings con
switch directamente. El enum da un numero fijo por cada tipo de comando,
strcmp hace la unica comparacion de texto necesaria, y el switch queda
simple de leer.

**Diferencia entre memoria disponible del sistema y memoria de un proceso?**
MemAvailable es cuanta memoria libre/recuperable tiene todo el sistema en
ese momento. VmRSS es cuanta memoria fisica tiene usando un proceso en
particular. Son escalas distintas.

**Para que malloc/free si se usan?** El unico malloc real del programa es el
que hace getline() internamente para el buffer de cada linea leida, y se
libera con free() cada vuelta del loop. No se maneja memoria dinamica propia
porque los trabajos se guardan en un arreglo de tamaño fijo.

**Que pasa si se hace exit con procesos activos?** No se espera
indefinidamente ni se los mata. Se avisa cuantos quedan corriendo y se les
deja seguir solos (quedan huerfanos, adoptados por init).
