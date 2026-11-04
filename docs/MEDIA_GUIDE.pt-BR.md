# Guia para incluir prints e vídeo de gameplay

[Voltar ao README](../README.pt-BR.md)

As capturas e o vídeo de demonstração já foram integrados ao kit e estão disponíveis em `assets/`:

| Arquivo gerado | Conteúdo | Formato |
| :--- | :--- | :--- |
| `assets/cover.webp` | Logotipo oficial do projeto com montanhas | Banner horizontal otimizado |
| `assets/gameplay.mp4` | Demonstração de gameplay capturada em iPhone | MP4 720p H.264 (~10 MB, faststart) |
| `assets/screenshots/01-launcher-portrait.webp` | Tela inicial do launcher em modo vertical | Vertical (iPhone) |
| `assets/screenshots/02-launcher-landscape.webp` | Launcher em modo horizontal | Horizontal widescreen |
| `assets/screenshots/03-touch-portrait.webp` | Controles por toque em modo vertical | Vertical |
| `assets/screenshots/04-title-touch.webp` | Tela de título com botões na tela | Horizontal |
| `assets/screenshots/05-career-menu.webp` | Menu de seleção de carreiras e modos | Horizontal |
| `assets/screenshots/06-gameplay.webp` | Gameplay ao vivo na pista de corrida | Horizontal |

Use `.webp` com qualidade boa para deixar o README leve. O GitHub mostra imagens armazenadas no repositório. Não exponha credenciais, dados pessoais, nome de Wi-Fi ou número de série do aparelho.

Quando os prints forem adicionados, substitua a seção "Screenshots and gameplay" do README inglês por:

```md
## Screenshots and gameplay

| Native iOS launcher | In-game touch controls |
| :---: | :---: |
| ![Launcher](assets/screenshots/01-launcher.webp) | ![Touch controls](assets/screenshots/03-touch.webp) |

| Gameplay | TV output |
| :---: | :---: |
| ![Gameplay](assets/screenshots/02-gameplay.webp) | ![HDMI or AirPlay](assets/screenshots/05-tv.webp) |

[Watch the gameplay video](LINK_DO_VIDEO_PUBLICADO)
```

No README em português, use a mesma tabela traduzida. Só adicione o link de vídeo depois que ele for publicado, por exemplo no YouTube; não coloque um link-placeholder quebrado no projeto. Para o Release, é possível anexar arquivos curtos de mídia, quando permitido pelo GitHub.

A arte de capa deste kit está em `assets/cover.webp` (otimizada em WebP a partir do logotipo oficial). Verifique previamente os direitos de uso dos elementos e marcas presentes na arte.
