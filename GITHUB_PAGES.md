# Publicar en GitHub Pages

Este proyecto ya esta preparado para Pages (estatico):

- `index.html` en raiz
- assets locales en `assets/`
- workflow en `.github/workflows/pages.yml`
- `.nojekyll` para servir todo tal cual

## 1) Inicializar repo local (si aun no existe)

```bash
git init
git add .
git commit -m "prepare github pages deployment"
git branch -M main
```

## 2) Crear repo en GitHub y subir

Opcion A (con GitHub CLI):

```bash
gh repo create <tu-repo> --public --source . --remote origin --push
```

Opcion B (manual en web):

1. Crea un repo publico vacio en GitHub.
2. Ejecuta:

```bash
git remote add origin https://github.com/<usuario>/<tu-repo>.git
git push -u origin main
```

## 3) Activar GitHub Pages

En GitHub:

- `Settings` -> `Pages`
- `Build and deployment` -> `Source`: **GitHub Actions**

Con eso, al hacer push a `main`, se publica automaticamente.

## URL final

Sera algo como:

`https://<usuario>.github.io/<tu-repo>/`

## Nota BLE

Web Bluetooth requiere navegador compatible (Chrome/Edge) y HTTPS.
GitHub Pages usa HTTPS, por lo que te sirve para probar BLE en internet.
