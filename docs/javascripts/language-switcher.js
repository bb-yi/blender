// Language Switcher for Bilingual Documentation
// Adds a global language toggle button at the top of the page

document.addEventListener('DOMContentLoaded', function () {
    // Language data
    const translations = {
        'nav': {
            'en': {
                'Home': 'Home',
                'Scene Extensions': 'Scene Extensions',
                'Extended Nodes': 'Extended Nodes',
                'NPR Workflow': 'NPR Workflow',
                'Interface & Settings': 'Interface & Settings',
            },
            'zh': {
                'Home': '首页',
                'Scene Extensions': 'Scene 级 Eevee 扩展',
                'Extended Nodes': '扩展着色器节点',
                'NPR Workflow': 'NPR Tree 工作流',
                'Interface & Settings': '界面与设置',
            }
        },
        'labels': {
            'en': '🇬🇧 English',
            'zh': '🇨🇳 中文'
        }
    };

    // Create language switcher button
    function createLanguageSwitcher() {
        const switcher = document.createElement('div');
        switcher.id = 'language-switcher';
        switcher.style.cssText = `
      position: fixed;
      top: 70px;
      right: 20px;
      z-index: 1000;
      background: var(--md-primary-fg-color);
      color: white;
      padding: 8px 16px;
      border-radius: 4px;
      cursor: pointer;
      font-weight: 500;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1);
      transition: all 0.3s ease;
    `;

        const currentLang = localStorage.getItem('docLanguage') || 'en';
        const otherLang = currentLang === 'en' ? 'zh' : 'en';
        switcher.textContent = translations.labels[otherLang];

        switcher.addEventListener('mouseenter', function () {
            this.style.opacity = '0.8';
            this.style.transform = 'scale(1.05)';
        });

        switcher.addEventListener('mouseleave', function () {
            this.style.opacity = '1';
            this.style.transform = 'scale(1)';
        });

        switcher.addEventListener('click', function () {
            toggleLanguage();
        });

        return switcher;
    }

    // Toggle language function
    function toggleLanguage() {
        const currentLang = localStorage.getItem('docLanguage') || 'en';
        const newLang = currentLang === 'en' ? 'zh' : 'en';
        localStorage.setItem('docLanguage', newLang);
        updatePageLanguage(newLang);
        updateNavigationLanguage(newLang);
        updateSwitcherText(newLang);
    }

    // Update page tabs
    function updatePageLanguage(lang) {
        // Find all tab buttons and content
        const tabButtons = document.querySelectorAll('[role="tab"]');
        const tabPanels = document.querySelectorAll('[role="tabpanel"]');

        let targetIndex = -1;

        // Find the target language tab
        tabButtons.forEach((btn, index) => {
            if (lang === 'en' && btn.textContent.includes('English')) {
                targetIndex = index;
            } else if (lang === 'zh' && btn.textContent.includes('中文')) {
                targetIndex = index;
            }
        });

        // Click the target tab if found
        if (targetIndex >= 0 && tabButtons[targetIndex]) {
            tabButtons[targetIndex].click();
        }
    }

    // Update navigation labels
    function updateNavigationLanguage(lang) {
        const navLabels = document.querySelectorAll('[data-i18n]');
        navLabels.forEach(label => {
            const key = label.getAttribute('data-i18n');
            if (translations.nav[lang][key]) {
                label.textContent = translations.nav[lang][key];
            }
        });

        // Also update navigation links without data-i18n attribute
        const navItems = document.querySelectorAll('nav a, .md-nav__link');
        navItems.forEach(item => {
            Object.keys(translations.nav[lang]).forEach(enKey => {
                const zhKey = translations.nav[lang][enKey];
                if (item.textContent.trim() === enKey || item.textContent.trim() === zhKey) {
                    item.textContent = zhKey;
                }
            });
        });
    }

    // Update switcher button text
    function updateSwitcherText(lang) {
        const switcher = document.getElementById('language-switcher');
        if (switcher) {
            const nextLang = lang === 'en' ? 'zh' : 'en';
            switcher.textContent = translations.labels[nextLang];
        }
    }

    // Apply saved language
    function applySavedLanguage() {
        const savedLang = localStorage.getItem('docLanguage');
        if (savedLang && savedLang !== 'en') {
            updatePageLanguage(savedLang);
            updateNavigationLanguage(savedLang);
        }
    }

    // Initialize
    const body = document.querySelector('body');
    if (body) {
        const switcher = createLanguageSwitcher();
        body.appendChild(switcher);

        // Apply saved language after delay to ensure DOM is ready
        setTimeout(() => {
            applySavedLanguage();
        }, 500);
    }
});
