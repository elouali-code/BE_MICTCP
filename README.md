# MIC_TCP Abderrahman El Ouali et Nathan Billard

## Différentes versions

Les différentes versions de notre mic_tcp se trouve dans le dossier **Versions**.
La version finale se trouve dans **src/mictcp.c**.
Pour tester, mettez la versions que vous souhaitez dans **src**, puis lancez les commandes

``` sh
make clean
make
```

Ensuite, en ouvrant 2 terminales (une pour le serveur et une pour le client) et tapez
``` sh
./build/server
./build/client
```
dans les terminales respectives.

Pour réduire les texte de débuggage, commentez la macro **#define DEBUG** dans le fichier **./src/mictcp.c** et recompilez.

## Synthèse

Vous pouvez trouver notre synthèse dans le fichier ![Rapport BE Réseau](./Rapport\ BE\ Réseau.pdf)
