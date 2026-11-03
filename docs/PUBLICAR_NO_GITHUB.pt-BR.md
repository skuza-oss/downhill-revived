# Como publicar este fork no GitHub

[Voltar ao README](../README.pt-BR.md)

**Repositório original:** https://github.com/skuza-oss/downhill-revived

**Conta GitHub prevista:** `ribeirogustav`

**Nome de fork sugerido:** `downhill-revived` (pode ser alterado depois, mas os links e o clone devem ser revisados).

## 1. Criar o fork

1. Entre no [projeto original](https://github.com/skuza-oss/downhill-revived).
2. Clique em **Fork**, no topo do GitHub.
3. Escolha sua conta `ribeirogustav`, mantenha o vínculo com o repositório original e crie o fork. Não crie um repositório vazio separado caso queira preservar o histórico e a identificação de fork.
4. Aguarde o repositório ser exibido na sua conta.

## 2. Incorporar as modificações iOS

**Importante:** a origem do GitHub não contém necessariamente todas as alterações para iOS que vieram no ZIP local. Não basta trocar o README e publicar o IPA. As alterações do código-fonte `port/ios/`, do core compartilhado e dos demais arquivos modificados devem ser efetivamente integradas ao seu fork, preservando a licença e os créditos.

Método seguro com Git, no computador:

```bash
git clone https://github.com/ribeirogustav/downhill-revived.git
cd downhill-revived

git switch -c ios-port-0.7.9
```

Em seguida, copie **somente os arquivos modificados e novos do port**, com base no ZIP local, para o diretório clonado. Não apague nem substitua a pasta `.git` do clone. Compare com `git status`, `git diff --stat` e `git diff` antes do commit. Preserve arquivos de upstream que não precisem mudar. Não adicione `iso/`, `asm/`, `assets/vutext.bin`, `build/`, certificados ou arquivos derivados de disco.

```bash
git status --short
git diff --stat
```

Confira o código com as referências e mudanças do upstream **0.7.9**, para não sobrescrever silenciosamente correções novas. O ZIP recebido indica ter essa base, mas não substitui uma revisão dos diffs.

## 3. Aplicar o kit de documentação

Copie os arquivos deste kit para **a raiz** do repositório do fork. Eles vão:

- **substituir** o `README.md` da raiz por uma apresentação internacional;
- **adicionar** `README.pt-BR.md` e `SHA256SUMS`;
- **adicionar** `assets/cover.webp`, `assets/gameplay.mp4`, novos guias sob `docs/` e o verificador em `scripts/`;
- **não substituir** `LICENSE`, `docs/CREDITS.md` nem os demais avisos e códigos existentes.

Observação: os caminhos `docs/BUILDING.md` e `docs/CREDITS.md` dos READMEs dependem dos arquivos originais do upstream que devem permanecer no fork. Se você trocar o nome do repositório, revise links absolutos de `git clone`.

## 4. Commit e publicação

```bash
git add -A
git status --short

# Inspecione com cuidado a lista antes do commit
git commit -m "feat(ios): publish native iOS port and documentation"
git push -u origin ios-port-0.7.9
```

No GitHub, revise a branch e, quando estiver pronta, integre-a na `main` do **seu fork**. Um pull request para o upstream só deve ser aberto se você quiser propor o port ao autor original.

## 5. Criar a primeira Release

1. Abra a aba **Releases** do seu fork.
2. Clique em **Draft a new release**.
3. Crie a tag `ios-v0.7.9` e o título **Downhill Revived for iOS v0.7.9 (Experimental)**.
4. Cole o texto de `RELEASE_NOTES_v0.7.9.md` do kit.
5. Anexe somente o artefato `DownhillRevived-iOS-0.7.9-build-70902-unsigned.ipa` e, se desejar, `SHA256SUMS`.
6. Confira a hash publicada. Deixe explícito que o IPA está **sem assinatura** e que a instalação exige assinatura Apple válida.
7. Marque **Set as a pre-release** por se tratar de um port experimental sem validação completa em aparelhos reais.
8. Publique a release somente quando o código-fonte correspondente já estiver no fork.

## 6. O que NÃO enviar

- ISO `.iso`, executável `SCUS_971.77`, dados extraídos, BIOS, músicas ou vídeos do jogo.
- `build/ios/DerivedData`, caches do compilador, senhas, perfis e chaves de assinatura.
- O ZIP inteiro com estrutura duplicada de pastas e o IPA solto dentro da raiz do repositório. Use **Releases** para o binário.

## 7. Conferência final

- [ ] O fork mantém a identificação do autor original no GitHub.
- [ ] O código iOS publicado corresponde ao IPA do Release.
- [ ] `LICENSE`, `docs/CREDITS.md` e avisos de terceiros estão preservados.
- [ ] `README.md` e `README.pt-BR.md` exibem a capa e os badges corretamente.
- [ ] A versão mínima `iOS 17.0` e a ISO americana `SCUS-97177` estão explícitas.
- [ ] O IPA está identificado como **unsigned**.
- [ ] SHA-256 do binário confere.
- [ ] Não existe nenhum arquivo protegido do disco no repositório.
- [ ] Testes físicos pendentes e limitações são descritos sem exageros.
