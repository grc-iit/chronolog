import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

const config: Config = {
  title: 'ChronoLog',
  tagline: 'A Distributed Shared Tiered Log Service for Large-Scale Science',
  favicon: 'img/favicon.ico',

  // Future flags, see https://docusaurus.io/docs/api/docusaurus-config#future
  future: {
    v4: true, // Improve compatibility with the upcoming Docusaurus v4
  },

  // Set the production url of your site here
  url: 'https://docs.chronolog.dev',
  baseUrl: '/',

  // GitHub pages deployment config.
  organizationName: 'grc-iit',
  projectName: 'ChronoLog',

  onBrokenLinks: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          editUrl:
            'https://github.com/grc-iit/ChronoLog/tree/main/docs/',
          lastVersion: '2.5.0',
          includeCurrentVersion: false,
          versions: {
            '2.5.0': {
              label: '2.5.0',
              path: '',
              badge: true,
            },
            '2.4.0': {
              label: '2.4.0',
              path: '2.4.0',
              badge: true,
            },
          },
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    image: 'img/chronolog-social-card.jpg',
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      logo: {
        alt: 'ChronoLog',
        src: 'img/chronolog-full-logo-transparent.webp',
        height: 64,
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'gettingStartedSidebar',
          position: 'left',
          label: 'Getting Started',
        },
        {
          type: 'docSidebar',
          sidebarId: 'architectureSidebar',
          position: 'left',
          label: 'Architecture',
        },
        {
          type: 'docSidebar',
          sidebarId: 'clientSidebar',
          position: 'left',
          label: 'Client',
        },
        {
          type: 'docSidebar',
          sidebarId: 'pluginsSidebar',
          position: 'left',
          label: 'Plugins',
        },
        {
          type: 'docSidebar',
          sidebarId: 'tutorialsSidebar',
          position: 'left',
          label: 'Tutorials',
        },
        {
          type: 'docSidebar',
          sidebarId: 'forDevelopersSidebar',
          position: 'left',
          label: 'For Developers',
        },
        {
          type: 'docsVersionDropdown',
          docsPluginId: 'default',
          position: 'right',
        },
        {
          href: 'https://chronolog.dev',
          label: 'Website',
          position: 'right',
        },
        {
          href: 'https://github.com/grc-iit/ChronoLog',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      logo: undefined,
      links: [
        {
          items: [
            {
              html: `
                <a href="https://chronolog.dev" class="footer__logo-link">
                  <img src="/img/chronolog-full-logo-transparent.webp" alt="ChronoLog logo" class="footer__logo-img" style="height: 5rem; border-radius: 0.25rem; margin-bottom: 1rem;" />
                </a>
                <p class="footer__description" style="font-size: 0.875rem; color: var(--ifm-footer-link-color); line-height: 1.625;">
                  A distributed and tiered shared log storage ecosystem. Funded by NSF (CSSI-2104013).
                </p>
              `,
            },
          ],
        },
        {
          title: 'Project',
          items: [
            {
              label: 'Architecture',
              href: 'https://chronolog.dev/architecture/',
            },
            {
              label: 'Use Cases',
              href: 'https://chronolog.dev/use-cases/',
            },
            {
              label: 'Documentation',
              to: '/docs/getting-started',
            },
            {
              label: 'GitHub',
              href: 'https://github.com/grc-iit/ChronoLog',
            },
          ],
        },
        {
          title: 'Gnosis Research Center',
          items: [
            {
              label: 'Website',
              href: 'https://grc.iit.edu',
            },
            {
              label: 'X (Twitter)',
              href: 'https://twitter.com/grc_iit',
            },
            {
              label: 'LinkedIn',
              href: 'https://www.linkedin.com/school/gnosis-research-center',
            },
            {
              label: 'grc@illinoistech.edu',
              href: 'mailto:grc@illinoistech.edu',
            },
          ],
        },
      ],
      copyright: `© ${new Date().getFullYear()} Gnosis Research Center, Illinois Institute of Technology`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
