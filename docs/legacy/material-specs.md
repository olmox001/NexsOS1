# Catalogo completo dei componenti UI di sistema

Design System · Material Design 3 / Material 3 Expressive · iOS Human Interface Guidelines · Desktop · Mobile · Touch

Per costruire l'interfaccia grafica di un sistema operativo serve una libreria di componenti UI (Widget Library) che comprenda non soltanto bottoni, ma anche controlli, menu, finestre, navigazione, input, notifiche, indicatori e componenti specifici del sistema.

Come riferimento utilizziamo:

- Google Material Design 3 (M3) e Material 3 Expressive per forme, colori, gerarchia, elevazione, animazioni e interazioni.
- Apple Human Interface Guidelines (HIG) per comportamenti iOS, touch, adattamento dello schermo e accessibilità.
- Estensioni OS-specific per desktop, window manager, file manager, taskbar, pannelli di sistema e controlli hardware.

Material 3 e le HIG Apple sono due sistemi distinti: il catalogo può condividere la stessa API semantica e adottare presentazioni diverse secondo la piattaforma.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developer.android.com\&sz=32)

Android Developers+2



## 1. Bottoni e controlli di azione

FilledTonalOutlinedText Elimina

&#x20;ImpostazioniDisabled

Anteprima funzionale indicativa. Le varianti definitive devono rispettare i token e gli stati del design system.

| Componente                  | Varianti necessarie                        |
| --------------------------- | ------------------------------------------ |
| Filled Button               | Primary, destructive, disabled             |
| Filled Tonal Button         | Standard, icon + label                     |
| Elevated Button             | Elevation normale e dinamica               |
| Outlined Button             | Standard, destructive                      |
| Text Button                 | Standard, compact                          |
| Icon Button                 | Standard, filled, tonal, outlined          |
| Toggle Icon Button          | On, off, mixed ove previsto                |
| Floating Action Button      | Small, regular, large                      |
| Extended FAB                | Icona + testo                              |
| Segmented Button            | Single-select, multi-select                |
| Split Button                | Azione primaria + menu secondario          |
| Menu Button                 | Popup, pull-down                           |
| Close / Minimize / Maximize | Window controls                            |
| Back / Forward              | Navigazione contestuale                    |
| Overflow Button             | Menu `•••`                                 |
| Disclosure Button           | Espandi / comprimi                         |
| Loading Button              | Indicatore di elaborazione interno         |
| Hold-to-confirm Button      | Conferma mantenendo premuto, estensione OS |
| Repeat Button               | Ripetizione azione durante pressione       |

## 2. Selezione, toggle e regolazione

&#x20;Wi-Fi

&#x20;Bluetooth

Selezione A

Selezione B

Chiaro

Scuro

Auto

63%

Componenti necessari: `Switch`, `Checkbox`, `TriStateCheckbox`, `RadioButton`, `RadioGroup`, `SegmentedControl`, `Slider`, `RangeSlider`, `StepSlider`, `DiscreteSlider`, `VerticalSlider`, `StepController`, `ToggleGroup`, `SelectionGroup`, `RatingControl`, `ColorPicker`, `OpacitySlider`, `BrightnessControl` e `VolumeControl`.

I componenti per luminosità, volume e colore possono condividere le primitive dei controlli standard, senza duplicarne il motore di interazione.

## 3. Input e immissione dati

| Famiglia           | Componenti                                        |
| ------------------ | ------------------------------------------------- |
| Testo              | TextField, OutlinedTextField, FilledTextField     |
| Testo avanzato     | MultilineTextField, TextArea, RichTextEditor      |
| Ricerca            | SearchField, SearchBar, SearchSuggestions         |
| Dati specializzati | PasswordField, NumericField, URLField, EmailField |
| Input strutturato  | MaskedField, OTPField, TokenField                 |
| Numeri             | NumberStepper, SpinBox                            |
| Scelta             | ComboBox, Select, Autocomplete                    |
| Date               | DatePicker, DateRangePicker, Calendar             |
| Orari              | TimePicker, TimeRangePicker                       |
| File               | FilePicker, DirectoryPicker, SaveDialog           |
| Colori             | ColorPicker, PalettePicker, GradientEditor        |
| Avanzati           | KeyBindingRecorder, ShortcutInput                 |

Comportamenti da supportare: focus, caret, selezione, copia/incolla, IME, testo composto, validazione, suggerimenti, annullamento/ripristino e accessibilità.

## 4. Navigazione e organizzazione

Impostazioni

GeneraleReteSistema

Preferenze generali

Componenti da includere:

- Top App Bar: small, center-aligned, medium, large, collapsing.
- Bottom App Bar e Bottom Navigation Bar.
- Navigation Rail, Navigation Drawer, Sidebar, Split View Sidebar.
- Tab Bar, Secondary Tabs, Scrollable Tabs, Vertical Tabs.
- Breadcrumbs, Back Stack, History Navigation.
- Pagination, Page Indicator, Stepper Navigation.
- Tree View, Hierarchical Navigation, Outline View.
- Toolbar, Command Bar, Contextual Action Bar.
- Search Navigation, Command Palette e Launcher Navigation.

Per il sistema desktop sono indispensabili anche navigation history, percorsi gerarchici, navigazione tramite tastiera e tab multipli.

## 5. Menu e azioni contestuali

| Componente          | Funzione                          |
| ------------------- | --------------------------------- |
| Dropdown Menu       | Elenco di opzioni                 |
| Context Menu        | Click destro o long press         |
| Nested Menu         | Sottomenu gerarchici              |
| Menu Bar            | Barra menu desktop                |
| Popup Menu          | Menu ancorato                     |
| Pull-down Menu      | Azioni da un controllo            |
| Command Menu        | Elenco comandi                    |
| Select Menu         | Selezione di un valore            |
| Radial Menu         | Menu circolare opzionale          |
| Action Sheet        | Scelta di azioni su mobile        |
| Edit Menu           | Taglia, copia, incolla, seleziona |
| Overflow Menu       | Azioni secondarie                 |
| Shortcut Menu       | Comandi con scorciatoie           |
| Dock / Taskbar Menu | Azioni delle applicazioni         |

Ogni voce dovrebbe supportare label, icona opzionale, shortcut, stato disabled, elemento selezionato, ruolo distruttivo e sottomenu.

## 6. Contenitori e strutture

Card

Filled, elevated, outlined

&#x20;List Item

Seconda riga

One, two, three-line

Catalogo: `Surface`, `Card`, `List`, `ListItem`, `Grid`, `DataTable`, `Tree`, `Carousel`, `Accordion`, `ExpansionPanel`, `Divider`, `GroupBox`, `Section`, `Panel`, `ScrollView`, `ScrollBar`, `Splitter`, `ResizablePane`, `DockablePanel`, `VirtualList`, `VirtualGrid`, `AdaptiveLayout`, `Scaffold`, `SafeAreaContainer`, `ResponsiveContainer`.

Le tabelle avanzate richiedono sorting, filtri, selezione multipla, resizing delle colonne, scrolling virtualizzato e navigazione da tastiera.

## 7. Dialoghi, finestre e overlay

Eliminare il file?

Questa operazione potrebbe non essere reversibile.

AnnullaElimina

Catalogo completo: `AlertDialog`, `ConfirmationDialog`, `FullScreenDialog`, `ModalDialog`, `ModelessDialog`, `BottomSheet`, `ModalBottomSheet`, `SideSheet`, `Popover`, `Tooltip`, `HoverCard`, `Toast`, `Snackbar`, `ContextPopover`, `ProgressDialog`, `FileDialog`, `PermissionDialog`, `AuthenticationDialog`, `WizardDialog`, `Window`, `Subwindow`, `FloatingWindow`, `PictureInPicture` e `DragPreview`.

Distinguere rigorosamente overlay modali e non modali, gestione focus, ordine Z, dismiss tramite Escape e comportamento durante il cambio di orientamento.

## 8. Feedback, notifiche e stato

Completato

Attenzione

Errore

12

Download

68%

Operazione completata

Dettagli

Componenti: `Badge`, `StatusBadge`, `StatusIcon`, `LinearProgress`, `CircularProgress`, `DeterminateProgress`, `IndeterminateProgress`, `BufferedProgress`, `Skeleton`, `Spinner`, `Snackbar`, `Toast`, `NotificationCard`, `NotificationGroup`, `NotificationHistory`, `ErrorBanner`, `WarningBanner`, `SuccessBanner`, `InlineValidation`, `EmptyState`, `OfflineState`, `RetryControl`, `ActivityIndicator` e `TaskProgress`.

A livello OS aggiungere anche notifiche persistenti, priorità, raggruppamento, azioni inline, progressi in background e notifiche silenziabili.

## 9. Chip, filtri ed elementi compatti

I quattro tipi fondamentali Material sono:

| Tipo            | Utilizzo                    |
| --------------- | --------------------------- |
| Assist Chip     | Azione contestuale          |
| Filter Chip     | Filtro attivabile           |
| Input Chip      | Valore inserito, rimovibile |
| Suggestion Chip | Suggerimento dinamico       |

Estensioni: `ClosableChip`, `AvatarChip`, `StatusChip`, `MultiSelectChip`, `Tag`, `Pill`, `Token`, `CompactActionChip`.

Sono utili in file manager, ricerca, impostazioni, notifiche e pannelli di sistema.

## 10. Componenti specifici del sistema operativo

Questa sezione è fondamentale: Material 3 non specifica una libreria completa per un sistema operativo desktop. I seguenti componenti devono essere definiti come estensioni coerenti con M3.

| Sottosistema    | Componenti necessari                                                             |
| --------------- | -------------------------------------------------------------------------------- |
| Window Manager  | TitleBar, WindowControls, ResizeHandle, WindowShadow, WindowSnap, WindowSwitcher |
| Desktop         | DesktopIcon, DesktopGrid, WallpaperView, DesktopContextMenu                      |
| Dock / Taskbar  | AppLauncher, AppIconButton, RunningIndicator, PinnedApp, TaskPreview             |
| System Status   | Clock, BatteryIndicator, NetworkIndicator, AudioIndicator, DeviceStatus          |
| Control Center  | QuickSettingsTile, BrightnessSlider, VolumeSlider, ConnectivityTile              |
| Notifications   | NotificationCenter, NotificationGroup, NotificationAction                        |
| Applications    | AppSwitcher, AppPreview, AppGrid, RecentApps                                     |
| File Manager    | FileItem, FolderItem, Breadcrumb, TreeView, PathBar, FilePreview                 |
| Storage         | DiskUsageIndicator, VolumeItem, MountStatus, TransferProgress                    |
| Security        | PermissionPrompt, CredentialPrompt, SessionLock, PrivacyIndicator                |
| Process Manager | ProcessRow, ResourceMeter, ProcessAction, PerformanceGraph                       |
| Hardware        | DeviceCard, DeviceStatus, ConnectionSelector, HardwareToggle                     |
| Session         | UserAvatar, UserSwitcher, LoginForm, LockScreen, PowerMenu                       |
| Accessibility   | AccessibilityOverlay, FocusIndicator, MagnifierControl                           |
| Boot / Recovery | BootProgress, BootChoice, RecoveryAction, ErrorScreen                            |

## 11. Media, grafica e visualizzazione

Un sistema completo dovrebbe includere `ImageView`, `IconView`, `Avatar`, `Thumbnail`, `ImageGallery`, `ZoomControl`, `PanControl`, `MediaPlayerControls`, `PlayPauseButton`, `SeekBar`, `Timeline`, `Waveform`, `SpectrumView`, `Chart`, `Graph`, `Canvas`, `ColorSwatch`, `PreviewPane` e `ThumbnailGrid`.

Per i componenti di sistema, i grafici delle prestazioni, gli indicatori CPU/RAM/GPU e i progressi di trasferimento devono usare la stessa infrastruttura di layout, rendering e accessibilità dei widget normali.

## 12. Stati obbligatori dei componenti

Non basta disegnare le varianti. Ogni widget interattivo deve implementare una macchina a stati coerente.

Anteprima stati di un pulsante

Azione primaria

Enabled / Normal

Selected

Disabled

Loading

Hover, pressed, focus e drag richiedono inoltre input reali da puntatore, tastiera o touch.

La matrice degli stati da prevedere comprende:

| Stato                | Significato                   |
| -------------------- | ----------------------------- |
| Enabled              | Utilizzabile                  |
| Hovered              | Puntatore sopra               |
| Focused              | Focus di navigazione          |
| Pressed              | Input attivo                  |
| Selected             | Selezionato                   |
| Checked              | Attivato                      |
| Indeterminate        | Selezione parziale            |
| Disabled             | Non disponibile               |
| Loading              | Operazione in corso           |
| Error                | Valore o operazione errata    |
| Dragged              | Trascinamento                 |
| Drop Target          | Destinazione di trascinamento |
| Expanded / Collapsed | Contenuto aperto o chiuso     |
| Read-only            | Consultazione senza modifica  |

Gli stati possono essere combinati quando semanticamente valido. Per esempio, un checkbox selezionato può anche essere focused o hovered.

## 13. Design tokens e regole di Google

I componenti devono essere realizzati sopra un sistema comune di token, invece di codificare direttamente colori, dimensioni e animazioni in ciascun widget. M3 definisce un sistema di theming per colori, tipografia e forme, esteso da Material 3 Expressive.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developer.android.com\&sz=32)

Android Developers+1



| Categoria     | Token necessari                                                                         |
| ------------- | --------------------------------------------------------------------------------------- |
| Color         | Primary, OnPrimary, Secondary, Tertiary, Surface, Error, Outline e rispettivi container |
| Typography    | Display, Headline, Title, Body, Label                                                   |
| Shape         | Corner radius, shape family e varianti per componente                                   |
| Elevation     | Livelli di elevazione, tonalità e ombre                                                 |
| Motion        | Durate, easing, transizioni, spring, gesture feedback                                   |
| State         | Opacità e layer visivi per hover, focus, pressed, disabled                              |
| Spacing       | Padding, gap, inset, margini                                                            |
| Size          | Dimensioni dei controlli, touch target e density                                        |
| Layout        | Breakpoint, safe area, vincoli e adattamento                                            |
| Iconography   | Libreria iconografica e metriche                                                        |
| Accessibility | Contrasto, focus, semantic role, descrizioni e navigazione                              |

### Regole da applicare

- Material 3 usa ruoli cromatici semantici, supporto dark/light e combinazioni di colori coerenti.

- Per il testo normale, puntare ad almeno 4,5:1 di contrasto e, per testo grande, 3:1, secondo WCAG AA.

- Su Android, Google raccomanda generalmente target interattivi di almeno 48 × 48 dp; su iOS Apple indica almeno 44 × 44 pt. Sono unità logiche diverse, non pixel fisici.&#x20;

  [image](https://www.google.com/s2/favicons?domain=https://developer.apple.com\&sz=32)

  Apple Developer Documentation+1

- La UI deve reagire a touch, mouse, tastiera, trackpad e, dove applicabile, penna.

- Focus e feedback visivo non devono dipendere esclusivamente dal colore.

- La riduzione del movimento e l'ingrandimento del testo devono essere supportati.

- I componenti devono gestire RTL, localizzazione e contenuti dinamici.

## 14. Compatibilità Material e iOS

Per evitare di mescolare comportamenti incompatibili, conviene distinguere semantica del controllo e presentazione per piattaforma.

| Componente semantico | Material 3               | iOS                         |
| -------------------- | ------------------------ | --------------------------- |
| Primary action       | Filled Button            | Prominent Button            |
| On/off               | Material Switch          | iOS Toggle                  |
| Tab navigation       | Navigation Bar           | Tab Bar                     |
| Main navigation      | Navigation Drawer / Rail | NavigationStack / Sidebar   |
| Option menu          | Dropdown Menu            | Menu / Context Menu         |
| Secondary modal      | Bottom Sheet             | Sheet                       |
| Confirmation         | Alert Dialog             | Alert / Confirmation Dialog |
| Selection group      | Segmented Button         | Segmented Control           |
| Search               | Search Bar               | Search Field                |
| Date selection       | Material Date Picker     | Native Date Picker          |
| App toolbar          | Top App Bar              | Navigation / Toolbar        |

Questa tabella è una mappatura architetturale, non un'equivalenza visiva perfetta. Su iOS è importante mantenere gesti, aree sicure, convenzioni di navigazione e feedback propri della piattaforma.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developer.apple.com\&sz=32)

Apple Developer Documentation+2



## 15. Organizzazione consigliata del catalogo

Per implementare una libreria di sistema realmente riutilizzabile, propongo una classificazione a livelli:

System UI

Desktop · Window Manager · Taskbar · Notifications · Control Center

Composite Widgets

Dialogs · Navigation · Forms · Tables · Menus · Sheets

Core Widgets

Buttons · Inputs · Selection · Text · Icons · Sliders

UI Foundation

Tokens · Layout · Input Events · Semantics · Motion · Rendering

L'architettura separa le primitive UI dai componenti compositi e dai servizi di sistema. Non richiede che il kernel conosca Material o iOS: il rendering e la gestione delle interazioni possono rimanere nel livello grafico userland.

## 16. Inventario di implementazione

Questa è una checklist delle famiglie principali per costruire e verificare la libreria.

Copertura delle famiglie UI

Seleziona le famiglie che hai già implementato.

0/11

Bottoni e azioni

\~18

Selezione e regolazione

\~16

Input e picker

\~22

Navigazione

\~18

Menu e comandi

\~14

Contenitori e layout

\~20

Dialog e overlay

\~20

Feedback e notifiche

\~20

Chip e filtri

\~9

Componenti OS

\~35

Media e grafica

\~20

Totale indicativo delle voci

## 212

Conteggio di pianificazione, non numero ufficiale di componenti Google. Le varianti, gli stati, i token e le combinazioni non sono conteggiati singolarmente.

&#x20;Copia checklist

## Documentazione ufficiale

- [Google Material Design 3 — Components](https://m3.material.io/components?utm_source=chatgpt.com)
- [Material Design 3 — Styles e design tokens](https://m3.material.io/styles?utm_source=chatgpt.com)
- [Android Developers — Catalogo Material 3](https://developer.android.com/develop/ui/compose/components?utm_source=chatgpt.com)
- [Apple — Human Interface Guidelines, Components](https://developer.apple.com/design/human-interface-guidelines/components/?utm_source=chatgpt.com)
- [Apple — Accessibilità](https://developer.apple.com/design/human-interface-guidelines/accessibility?utm_source=chatgpt.com)

Per una UI di sistema completa, questo catalogo costituisce una base di circa 200 voci tra componenti standard, varianti funzionali ed estensioni OS-specific. Il passo progettuale importante è assegnare a ogni voce un contratto preciso: proprietà, eventi, stati, input supportati, geometria, token Material, adattamento iOS e criteri di test. Solo così il catalogo diventa una specifica implementabile e non una semplice raccolta di disegni.

# Sistema grafico UI — Material Hydroglass

Catalogo asset open source · Standard grafico · Superfici · Materiali · Rendering GPU · Desktop / Mobile

Propongo di sviluppare un linguaggio visivo denominato provvisoriamente Material Hydroglass, costruito sopra Material Design 3, ma con una personalità specifica per il sistema operativo.

L'obiettivo è combinare quattro qualità:

- Material: componenti geometricamente coerenti, leggibili, accessibili e facilmente renderizzabili.
- Hydrogel e vetro: superfici traslucide, profondità controllata, rifrazione suggerita e riflessi morbidi.
- Plastica scura raffinata: materiale principale dei pannelli, dei pulsanti e delle finestre.
- Metallo satinato: sottilissimi dettagli argento e oro caldo, usati soltanto su bordi, riflessi e componenti selezionati.

Lo standard dovrà mantenere un'identità consistente tanto su un display Retina quanto su un monitor desktop standard, utilizzando primitive vettoriali, shader GPU e una quantità limitata di texture.

## 1. Riferimenti visivi ufficiali

[Material 3 Expressive](https://images.openai.com/static-rsc-4/2semUutpcCh-NIzlW407YC-Ty1MR6tpiYR67HDNPd11LgdEz8VCCHTepzb2KXY5buxyDHoBtnu-A3ETmmBj3Al7dHw0W8xuOke2FLC5aMb5tKWMoXND2fAqyaiayn2UzpD3Vv3GCPV2OaUzYZ5bhuMCtB7dd-_M5ZOj_H-heor0?purpose=inline)

Material 3 Expressive

Forma, gerarchia, stati e animazioni dei componenti.

[Specifiche Google](https://m3.material.io/?utm_source=chatgpt.com)

[GitHub - slint-ui/material-components · GitHub](https://images.openai.com/static-rsc-4/_yMharhtFgTfe42svnt7bagbNS6ROFry-QGXUrxyvCMC8pXJ90Y7XIQ8u67o7Rb8uQLoc5q5xQhxwcHY6M_jlAwpkQo6qXaU4DZq-1r1li5VYKkntA0lJQp-_LZ8wksAT66UCpto26RU5RWyfIjGxCCvAoq-eFkvTIZLYmMBQe4?purpose=inline)

Material 3 Dark

Ruoli cromatici, tonalità di superficie e contrasto.

[Color system](https://m3.material.io/styles/color/overview?utm_source=chatgpt.com)

Queste sono fonti visive di riferimento. Le specifiche Google sono pubbliche, ma questo non significa che ogni immagine, mockup o file Figma sia liberamente redistribuibile. Per incorporare risorse nell'OS utilizzeremo asset con licenza esplicitamente compatibile.

## 2. Risorse grafiche e codice ufficiali open source

[Material Symbols Vs Icons at Lyn Romano blog](https://images.openai.com/static-rsc-4/GzpqJLCJ9apLRsRslD4-TDMay31BMPdFlZMGP2BFGDx6ezDpR_bjoDGs5UitTI3bfDDwuo_IZapjl-8QamMgD91vIv3W082JGkwInlt2LNgRXbqi9hhyBJ2lmcWzn9GdgSfjLqjPRifEbbVoglXjrpTf4MyK96WahpUN8s5pt4A?purpose=inline)

Google Material Symbols

Apache 2.0

Libreria principale di icone di sistema. Varianti outlined, rounded e sharp, assi variabili per peso, riempimento e dimensione ottica. SVG e font vettoriali.

[Catalogo icone](https://fonts.google.com/icons?utm_source=chatgpt.com) · [Repository GitHub](https://github.com/google/material-design-icons?utm_source=chatgpt.com)

[Material 3 Design Kit for Figma - Free UI Kit - UI STORE DESIGN](https://images.openai.com/static-rsc-4/hG4hUHZGGYL0F7a75EsB2zN8Y9oyTUbVQqYBntkYz7J_mmuktGHOYuc3CF5NUZLcVlV0jKazEGOmkeN_RGU485qB0RYkRqasr10wCLrsP_VTdSzl0gVZLAyR-yTprj108juZ3fmCEAMhB2_BjYpwNrE88xcujlWnpOrcgQvFD5I?purpose=inline)

Material 3 Design Kit

Risorsa di progettazione

Componenti, stili, layout e riferimenti dimensionali. Ottimo per affiancare al catalogo le rappresentazioni ufficiali. La licenza di ogni kit va verificata prima di ridistribuirne i file.

[Portale Material 3](https://m3.material.io/?utm_source=chatgpt.com)

[Google Roboto Flex – Chris does Content](https://images.openai.com/static-rsc-4/uVsOu3LdILKbPzNDBI9XUISGAWr9Qe7K0fVkbLAw8lBlJpDGpgHBJtldeQqpY5NQgLwQVmFJ_QdbYzLPXGIejhUdbFHjyR5Lggbz3O-58jXV_5RmYfEqec1a3R0YZBYQ83ov7ceKL49U_guMSxvlHW1nHmCXM2_9AsKVYjbFEAo?purpose=inline)

Roboto Flex

OFL 1.1

Font principale consigliato: variabile, con controllo del peso, della larghezza e della dimensione ottica. Ideale per pannelli compatti e interfacce scalabili.

[Repository e licenza](https://github.com/googlefonts/roboto-flex?utm_source=chatgpt.com)

[Consenti agli utenti di personalizzare la loro esperienza cromatica nella tua app  |  Views  |  Android Developers](https://images.openai.com/static-rsc-4/GAoTGM_kvNdZVAI1w_afYyx433lHdLK5jQjH8P9VxV7s9T6fK34TBc6b9Xbl4TFVfUwc7W5558LEPicuo_LkDpm0kU9PjcrDaWBDURAKzQxSWchDIabXeKQkTo4LUbG1MMiCe-LPrBi1feT-fQX4efvh_6kqdhX9hdh6nuW2yu0?purpose=inline)

Material Color Utilities

Apache 2.0

Libreria di Google per generare tonalità e schemi dinamici con ruoli cromatici Material. Utilizzabile per wallpaper colors, temi e accessibilità.

[Repository ufficiale](https://github.com/material-foundation/material-color-utilities?utm_source=chatgpt.com)

[Uno Material](https://images.openai.com/static-rsc-4/NXBEbeO6OqLgMA7AaGs4IMZNCJ2eVoRK1AbSb0_a6SUqoLLZXpQkMeDcSLMFYjgZWoMjemyNmryeSSPdoqLpsUjfMn3WxxJo1rCmi1W3kFadpPTw4Gv30NoXSjNRPVOXvATrzm5OU8SJGiw2dp4SYJ27de6q6kb_Ix-Vx4mDL48?purpose=inline)

Material Web

Apache 2.0

Implementazioni consultabili di button, switch, text field, menu, dialog e altre primitive. Utili come riferimento per stati, struttura e token. Il repository è attualmente in maintenance mode.

[Codice e componenti](https://github.com/material-components/material-web?utm_source=chatgpt.com)

[Migrate from Material 2 to Material 3 in Compose  |  Jetpack Compose  |  Android Developers](https://images.openai.com/static-rsc-4/OQpAr8TfsRWCdJb2bg_dFe94eNAtiGTtnZ9qEFmHD_8tZ1knVDpM_IR2IMmGvpzarZp2jUQiNZjeYJQJuy9TN2lg3bkOrkBYlONykiWof_s3przI71QcFiqkY-Bgh5uKjzfiCrFZDUdvOoO23RAldieKf7eAlfxRCwFjupc2zV4?purpose=inline)

Jetpack Compose Material 3

Apache 2.0

Riferimento implementativo moderno per componenti Material, semantica, comportamento e accessibilità. Da studiare e adattare, non da imporre come dipendenza dell'OS.

[Catalogo](https://developer.android.com/develop/ui/compose/components?utm_source=chatgpt.com) · [Codice sorgente](https://cs.android.com/androidx/platform/frameworks/support/+/androidx-main:compose/material3/?utm_source=chatgpt.com)

Le licenze e lo stato dei progetti sono verificabili nelle rispettive fonti.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developers.google.com\&sz=32)

Google for Developers+4



Per iOS: le Apple Human Interface Guidelines e gli asset di Apple sono validi riferimenti di comportamento, ma non vanno trattati automaticamente come risorse open source. In particolare, SF Symbols non è una sostituzione universalmente redistribuibile di Material Symbols per un OS indipendente.

## 3. Le superfici: vetro, hydrogel, plastica e metallo

La gerarchia dei materiali determina profondità e riconoscibilità dell'interfaccia.

Dark polymer

Materiale base, opaco e morbido.

Hydrogel

Traslucido, con riflesso diffuso.

Smoked glass

Vetro fumé, scuro e profondo.

Wet surface

Effetto acqua, luminosità sottile.

Campioni cromatici indicativi. Blur, rifrazione, Fresnel e specular highlight andranno implementati nel renderer, non simulati con colori piatti.

### Catalogo dei materiali

| Materiale      | Applicazioni                      | Trattamento                              |
| -------------- | --------------------------------- | ---------------------------------------- |
| Polymer Matte  | Desktop, sfondi, pannelli di base | Opaco, rumore finissimo                  |
| Polymer Satin  | Bottoni, menu, toolbar            | Luce diffusa morbida                     |
| Hydrogel Soft  | Slider, switch, controlli attivi  | Traslucenza e riflesso arrotondato       |
| Hydrogel Clear | Elementi flottanti                | Trasparenza elevata, controllo contrasto |
| Smoked Glass   | Finestre e sidebar                | Vetro fumé con blur controllato          |
| Frosted Glass  | Overlay, notifiche                | Vetro satinato con blur                  |
| Wet Glass      | Accenti selezionati               | Riflessi morbidi e profondità            |
| Silver Edge    | Bordi e separatori                | Argento desaturato                       |
| Warm Gold Edge | Focus premium e dettagli          | Oro tenue e satinato                     |

La combinazione più efficace è utilizzare plastica scura opaca per la struttura e vetro hydrogel soltanto per gli elementi che devono apparire sospesi. Evita che la trasparenza renda indistinguibili superfici, testo e finestre.

## 4. Palette cromatica iniziale

Questa è una palette progettuale originale, non una palette ufficiale Google. Il ruolo dei colori rimane compatibile con l'architettura dei token Material 3.

Obsidian

\#0C1015

Graphite

\#151B23

Midnight

\#222D38

Slate Glass

\#344653

Hydro Blue

\#8ABCCB

Ice Silver

\#C4D0D8

Champagne

\#D1B98B

Snow

\#F1F5F8

Seleziona un campione per copiare il codice HEX.

### Regole dei colori

La distribuzione cromatica consigliata è:

- 75–85% superfici neutre scure;
- 10–20% superfici hydrogel e accenti cromatici;
- non oltre il 5% di dettagli metallici evidenti.

Queste percentuali sono obiettivi estetici, non prescrizioni Material 3.

L'oro non deve sostituire il colore di errore, di selezione o di navigazione. I ruoli funzionali devono mantenere colori riconoscibili, con tonalità dinamiche e contrasto verificato.

## 5. Specifica delle trasparenze

Solid polymer

100% opacity

Smoked glass

92% opacity

Hydrogel panel

82% opacity

Floating glass

65% opacity

Clear overlay

45% opacity

Valori di partenza proposti per il tint delle superfici, da regolare insieme allo sfondo e alla leggibilità.

| Proprietà             | Profilo iniziale suggerito               |
| --------------------- | ---------------------------------------- |
| Surface alpha         | 0.75–1.00                                |
| Glass tint alpha      | 0.45–0.92                                |
| Highlight alpha       | 0.04–0.18                                |
| Stroke alpha          | 0.08–0.30                                |
| Metal highlight alpha | 0.10–0.28                                |
| Blur radius           | 0–24 px fisici, secondo qualità e scala  |
| Shadow radius         | 4–32 px fisici                           |
| Shadow alpha          | 0.12–0.30                                |
| Internal reflection   | Gradiente breve, poco contrastato        |
| Refraction            | Solo elementi selezionati e GPU adeguate |

Importante: opacity e blur non sono equivalenti. La trasparenza determina quanta informazione sottostante è visibile; il blur ne attenua la struttura. Per mantenere leggibilità, il testo deve essere composto su uno sfondo con contrasto misurabile anche quando cambia il contenuto sottostante.

## 6. Dimensioni, densità, pixel e DPI

Una UI di sistema non dovrebbe utilizzare direttamente il DPI fisico per decidere tutte le dimensioni. Serve una geometria logica indipendente dal framebuffer.

### Unità fondamentali

| Unità        | Definizione                                 |
| ------------ | ------------------------------------------- |
| px           | Pixel fisici nel framebuffer                |
| dp           | Unità Android indipendente dalla densità    |
| sp           | Unità Android scalata anche per il testo    |
| pt           | Punto logico delle interfacce Apple         |
| dpi / ppi    | Densità fisica di stampa o del display      |
| Scale factor | Conversione da unità logiche a pixel fisici |

Google basa `dp` su 160 dpi di riferimento e raccomanda griglie di 4 e 8 unità. Apple utilizza scale factor @1x, @2x e @3x per le immagini raster.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developer.android.com\&sz=32)

Android Developers+1



### Esempio di conversione

Scala del display

2×

1×1.25×1.5×2×3×

Dimensione logica

24

# 48 px

24 unità logiche × 2

Questa conversione assume che lo scale factor sia già stato calcolato dal sistema di layout; non implica che `1 dp = 1 pt` su tutti i dispositivi.

### Dimensioni standard suggerite

| Elemento              | Dimensione logica iniziale |
| --------------------- | -------------------------- |
| Icona piccola         | 16                         |
| Icona standard        | 24                         |
| Icona grande          | 32                         |
| Icona desktop         | 48–64                      |
| Controllo compatto    | 32–36                      |
| Button normale        | 40                         |
| Button touch          | 48–56                      |
| Menu item             | 40–48                      |
| Toolbar               | 48–64                      |
| List row              | 48–72                      |
| Window titlebar       | 40–48                      |
| Corner radius piccolo | 4–8                        |
| Corner radius medio   | 12–16                      |
| Corner radius grande  | 20–28                      |

Sono valori iniziali del nostro sistema, non quote universali Material.

Per i target touch, adottare almeno 48 dp per le superfici interattive Android e mirare a 44 pt su iOS. Le HIG Apple moderne distinguono dimensioni predefinite e minime per piattaforma; non occorre rendere fisicamente grandi tutti i controlli desktop quando si usa il mouse.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://developer.apple.com\&sz=32)

Apple Developer Documentation+1



## 7. Geometria, curvature e bordi

Per ottenere il materiale hydrogel, la geometria è importante quanto la trasparenza.

Small

Icon buttons

Medium

Cards / Input

Large

Panels

Full

Pills

Soft

Menus

Floating

Overlays

Per le superfici morbide consiglio di supportare:

- Rounded rectangles tradizionali.
- Continuous corners / superellipse, quando il componente lo richiede.
- Bordo interno e bordo esterno indipendenti.
- Specular highlight concentrato su uno o due lati.
- Ombre morbide non uniformi.
- Shape morphing parametrico per transizioni e stati.

Il rendering ideale è vettoriale o procedurale: le forme non richiedono un PNG diverso per ciascun livello di scala.

## 8. Standard di rendering GPU

Per massimizzare qualità e prestazioni, i materiali vanno definiti come parametri e applicati da shader riutilizzabili.

UI Scene Graph

Layout · Geometry · Clipping · Z-order

Material Resolver

Tokens · Opacity · Shape · Lighting

Opaque

Glass

Metal

GPU Compositor

Batching · Blur caches · Damage tracking

Display Surface

Color space · Scale · Presentation

### Tecniche e priorità

| Tecnica                      | Utilizzo                      | Priorità  |
| ---------------------------- | ----------------------------- | --------- |
| Signed Distance Fields       | Bordi, forme, curve           | Alta      |
| GPU instancing / batching    | Pulsanti e primitive ripetute | Alta      |
| Premultiplied alpha          | Composizione trasparente      | Alta      |
| Scissor / stencil clipping   | Finestre e superfici          | Alta      |
| Damage tracking              | Ridisegno parziale            | Alta      |
| Cached backdrop blur         | Vetro e overlay               | Alta      |
| Separable / downsampled blur | Sfocatura efficiente          | Alta      |
| Gradient LUT                 | Riflessi e materiali          | Media     |
| Specular highlight shader    | Hydrogel e metallo            | Media     |
| Normal map                   | Dettagli di superficie        | Opzionale |
| Screen-space refraction      | Effetti acqua selezionati     | Opzionale |
| Procedural micro-noise       | Finitura polimerica           | Opzionale |

### Regole prestazionali

Il blur non deve essere ricalcolato indistintamente su tutto lo schermo ogni frame. Conviene riutilizzare la texture dello sfondo, tracciare le regioni modificate, ridurre la risoluzione dei passaggi di sfocatura e ricomporre soltanto le regioni interessate.

I materiali possono avere profili di qualità:

| Profilo  | Trasparenza    | Blur                   | Rifrazione  |
| -------- | -------------- | ---------------------- | ----------- |
| Economy  | Alpha blending | Statico / cache        | Disattivata |
| Balanced | Alpha + tint   | Caching regionale      | Selettiva   |
| High     | Completa       | Multi-pass ottimizzato | Limitata    |
| Ultra    | Completa       | Alta qualità           | Avanzata    |

L'implementazione deve mantenere identica geometria, input e semantica in ogni profilo. Il fallback CPU può ridurre gli effetti senza modificare il comportamento dei widget.

## 9. Standard dei display

La libreria deve supportare display piccoli, Retina, monitor ad alta densità, schermi esterni e configurazioni multimonitor.

| Caratteristica  | Requisito                                          |
| --------------- | -------------------------------------------------- |
| Scale           | 1×, 1.25×, 1.5×, 2×, 3× e valori intermedi         |
| Layout          | Adattivo alla dimensione logica                    |
| Resolution      | Indipendente dalla risoluzione fisica              |
| Multi-monitor   | Scale factor e color space per display             |
| Rotation        | Ricalcolo layout e safe areas                      |
| Aspect ratio    | Nessuna proporzione fissa obbligatoria             |
| Color           | sRGB baseline                                      |
| Wide color      | Display P3 quando disponibile                      |
| HDR             | Pipeline opzionale, con SDR corretto               |
| Alpha           | Composizione premoltiplicata                       |
| Refresh rate    | Rendering indipendente dalla frequenza del display |
| Animation       | Sincronizzata alla presentazione                   |
| Pixel alignment | Rasterizzazione nitida a ogni scala                |

Per il rendering in alta qualità, gli elementi vettoriali devono essere rasterizzati alla scala effettiva; il testo deve usare un sistema di font shaping completo, non sprite di caratteri pre-generati.

In una pipeline GPU moderna sono importanti anche gestione corretta dello spazio colore, compositing in spazio appropriato e conversione finale verso il display.

## 10. Mappatura delle risorse al catalogo dei widget

Questo è il collegamento operativo tra il catalogo precedente e gli asset da raccogliere.

| Famiglia UI         | Risorsa principale      | Asset aggiuntivi                  |
| ------------------- | ----------------------- | --------------------------------- |
| Buttons             | Material 3 Components   | Material Symbols                  |
| Selection           | Material 3 Components   | Check, radio, state geometry      |
| Input               | Material 3 + Compose    | Cursor, handles, validation icons |
| Navigation          | Material 3 Components   | Frecce, sidebar, breadcrumbs      |
| Menus               | Material 3 + Compose    | Indicatori, submenu, shortcut     |
| Cards / Layout      | Material 3 Design Kit   | Material shaders                  |
| Dialogs             | Material 3 Components   | Warning / status icons            |
| Notifications       | Material Symbols        | Notification surfaces             |
| Chips               | Material 3 Components   | Selection state icons             |
| Window Manager      | Componenti OS originali | Window controls, grips            |
| Desktop / Taskbar   | Componenti OS originali | App icons, status icons           |
| File Manager        | Material Symbols        | MIME icons, thumbnails            |
| System Settings     | Material Symbols        | Device iconography                |
| Media               | Material Symbols        | Playback controls, waveform       |
| Debug / Performance | Widget originali        | Graph primitives, gauges          |

Per il file manager e il desktop servirà inoltre un set originale di icone applicazione, cartella, volume e MIME. Material Symbols fornisce buona parte dei simboli funzionali, ma non sostituisce l'intero tema iconografico di un sistema operativo.

## 11. Risorse complementari open source

Questi progetti sono utili per coprire le parti non completamente risolte dal catalogo ufficiale Google.

| Progetto                                                                                                           | Utilizzo                   | Licenza    |
| ------------------------------------------------------------------------------------------------------------------ | -------------------------- | ---------- |
| [Material Symbols](https://github.com/google/material-design-icons?utm_source=chatgpt.com)                         | Iconografia principale     | Apache 2.0 |
| [Material Color Utilities](https://github.com/material-foundation/material-color-utilities?utm_source=chatgpt.com) | Palette dinamiche          | Apache 2.0 |
| [Roboto Flex](https://github.com/googlefonts/roboto-flex?utm_source=chatgpt.com)                                   | Tipografia                 | OFL 1.1    |
| [Material Web](https://github.com/material-components/material-web?utm_source=chatgpt.com)                         | Riferimento componenti     | Apache 2.0 |
| [Skia](https://github.com/google/skia?utm_source=chatgpt.com)                                                      | Rendering grafico 2D       | BSD-style  |
| [Filament](https://github.com/google/filament?utm_source=chatgpt.com)                                              | Studio dei materiali PBR   | Apache 2.0 |
| [Vulkan Samples](https://github.com/KhronosGroup/Vulkan-Samples?utm_source=chatgpt.com)                            | Pipeline GPU e prestazioni | Apache 2.0 |
| [LunaSVG](https://github.com/lunasvg/lunasvg?utm_source=chatgpt.com)                                               | Parsing e rendering SVG    | MIT        |

Skia, Filament e Vulkan Samples sono riferimenti tecnici, non dipendenze obbligatorie. Prima di importare codice o asset occorre verificare le licenze dei file effettivamente selezionati, inclusi eventuali asset di terze parti.

## 12. Standard di consegna per ogni widget

Ogni componente del catalogo dovrebbe avere una scheda contenente:

Esempio: Button / Filled

Specifica proposta

&#x20;Conferma

Material: Polymer Satin · Accent: Hydro Blue

| Dimensione   | 40 unità logiche, profilo normale                |
| ------------ | ------------------------------------------------ |
| Touch target | 48 dp Android / 44 pt iOS                        |
| Corner       | Full / pill                                      |
| Icona        | Material Symbols, 20–24 unità                    |
| Materiale    | Polymer Satin                                    |
| Stati        | Normal, hover, pressed, focus, disabled, loading |
| Output       | Vector geometry + material parameters            |

Per ogni widget è opportuno conservare le risorse sorgenti, i token, le geometrie, i test di rendering alle diverse scale e la relativa licenza. Le bitmap dovrebbero essere riservate agli elementi che non possono essere rappresentati efficacemente con vettori o shader.

## Standard risultante

La direzione progettuale è quindi Material 3 per usabilità e componenti, Material Hydroglass per le superfici e un renderer vettoriale/GPU per qualità e prestazioni.

Le decisioni centrali sono: fondi scuri neutri, hydrogel azzurro-grigio, superfici opache prevalenti, vetro soltanto dove aggiunge gerarchia, metallo oro/argento in tracce minime, scale indipendenti dal DPI e asset ufficiali open source quando disponibili.

Il catalogo grafico definitivo potrà così essere suddiviso in tre insiemi chiaramente verificabili: asset ufficiali riutilizzabili, specifiche ufficiali di riferimento e risorse originali del sistema operativo. Questa distinzione protegge sia la coerenza tecnica sia la correttezza delle licenze.