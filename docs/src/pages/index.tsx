import type {ReactNode} from 'react';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';

import styles from './index.module.css';

function Hero() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={styles.heroBanner}>
      <div className="container">
        <Heading as="h1" className={styles.heroTitle}>
          {siteConfig.title}
        </Heading>
        <p className={styles.heroTagline}>{siteConfig.tagline}</p>
        <div className={styles.buttons}>
          <Link className={styles.btnPrimary} to="/docs/getting-started/overview">
            Get Started
            <svg className={styles.btnIcon} viewBox="0 0 20 20" fill="currentColor">
              <path fillRule="evenodd" d="M3 10a.75.75 0 01.75-.75h10.638l-3.96-3.72a.75.75 0 011.04-1.06l5.25 4.93a.75.75 0 010 1.1l-5.25 4.93a.75.75 0 01-1.04-1.06l3.96-3.72H3.75A.75.75 0 013 10z" clipRule="evenodd" />
            </svg>
          </Link>
          <Link className={styles.btnOutline} href="https://github.com/grc-iit/ChronoLog">
            <svg className={styles.btnIcon} viewBox="0 0 16 16" fill="currentColor">
              <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z" />
            </svg>
            GitHub
          </Link>
        </div>
      </div>
    </header>
  );
}

type DocSection = {
  title: string;
  link: string;
  description: string;
  icon: ReactNode;
};

const docSections: DocSection[] = [
  {
    title: 'Getting Started',
    link: '/docs/getting-started/overview',
    description: 'Installation, core concepts, and your first deployment.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M12 2L12 14M12 14L8 10M12 14L16 10" />
        <path d="M4 20h16" />
      </svg>
    ),
  },
  {
    title: 'User Guide',
    link: '/docs/user-guide/overview',
    description: 'Architecture, data model, configuration, and operations.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M4 19.5A2.5 2.5 0 016.5 17H20" />
        <path d="M6.5 2H20v20H6.5A2.5 2.5 0 014 19.5v-15A2.5 2.5 0 016.5 2z" />
      </svg>
    ),
  },
  {
    title: 'Client API',
    link: '/docs/client/overview',
    description: 'C++, Python, and CLI reference with examples.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <polyline points="16 18 22 12 16 6" />
        <polyline points="8 6 2 12 8 18" />
      </svg>
    ),
  },
  {
    title: 'Plugins',
    link: '/docs/plugins/overview',
    description: 'ChronoKVS, ChronoStream, and extensibility.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83" />
      </svg>
    ),
  },
  {
    title: 'Tutorials',
    link: '/docs/tutorials/overview',
    description: 'Docker single-node, multi-node, and native Linux walkthroughs.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <rect x="2" y="3" width="20" height="14" rx="2" ry="2" />
        <line x1="8" y1="21" x2="16" y2="21" />
        <line x1="12" y1="17" x2="12" y2="21" />
      </svg>
    ),
  },
  {
    title: 'Contributing',
    link: '/docs/contributing/overview',
    description: 'Development setup, guidelines, and workflow.',
    icon: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
        <circle cx="18" cy="18" r="3" />
        <circle cx="6" cy="6" r="3" />
        <path d="M6 21V9a9 9 0 009 9" />
      </svg>
    ),
  },
];

function DocSectionsGrid() {
  return (
    <section className={styles.docSections}>
      <div className="container">
        <div className={styles.docGrid}>
          {docSections.map((section) => (
            <Link
              key={section.title}
              className={styles.docCard}
              to={section.link}>
              <div className={styles.docCardIcon}>{section.icon}</div>
              <div className={styles.docCardTitle}>{section.title}</div>
              <p className={styles.docCardDesc}>{section.description}</p>
            </Link>
          ))}
        </div>
      </div>
    </section>
  );
}

export default function Home(): ReactNode {
  return (
    <Layout
      title="Docs"
      description="ChronoLog documentation — installation, API reference, tutorials, and guides for the distributed shared tiered log service.">
      <Hero />
      <main>
        <DocSectionsGrid />
      </main>
    </Layout>
  );
}
