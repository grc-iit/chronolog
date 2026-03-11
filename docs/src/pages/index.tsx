import type {ReactNode} from 'react';
import clsx from 'clsx';
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
          <Link
            className="button button--primary button--lg"
            to="/docs/getting-started/overview">
            Get Started
          </Link>
        </div>
      </div>
    </header>
  );
}

function StatsBar() {
  const stats = [
    'v2.4.0',
    'Distributed',
    'HPC-Scale',
    'Multi-Tiered',
    'Open Source',
  ];
  return (
    <div className={styles.statsBar}>
      <div className="container">
        <div className={styles.statsRow}>
          {stats.map((s) => (
            <span key={s} className={styles.statBadge}>{s}</span>
          ))}
        </div>
      </div>
    </div>
  );
}

function ArchitectureSection() {
  return (
    <section className={styles.section}>
      <div className="container">
        <Heading as="h2" className={styles.sectionHeading}>
          Architecture
        </Heading>
        <p className={styles.sectionSubheading}>
          A layered design separating ingestion, storage, and retrieval for maximum scalability
        </p>
        <img
          src="/img/ChronoLogDesign.png"
          alt="ChronoLog architecture diagram"
          className={styles.archImg}
        />
        <p className={styles.archCaption}>
          ChronoLog system architecture — ChronoVisor, ChronoKeeper, ChronoGrapher, and ChronoPlayer layers
        </p>
      </div>
    </section>
  );
}

type QuickStartItem = {icon: string; title: string; desc: string; link: string; label: string};

const quickStarts: QuickStartItem[] = [
  {
    icon: '🐳',
    title: 'Single Node',
    desc: 'Spin up a complete ChronoLog stack on a single machine in minutes using Docker Compose.',
    link: '/docs/tutorials/docker-single-node/running-chronolog',
    label: 'Single Node',
  },
  {
    icon: '🌐',
    title: 'Multi Node',
    desc: 'Deploy a distributed ChronoLog cluster across multiple hosts with Docker networking.',
    link: '/docs/tutorials/docker-multi-node/running-chronolog',
    label: 'Multi Node',
  },
  {
    icon: '📖',
    title: 'Full Documentation',
    desc: 'Explore the complete reference: architecture, configuration, API, and advanced topics.',
    link: '/docs/getting-started/overview',
    label: 'Browse Docs',
  },
];

function QuickStartSection() {
  return (
    <section className={styles.sectionAlt}>
      <div className="container">
        <Heading as="h2" className={styles.sectionHeading}>
          Quick Start
        </Heading>
        <p className={styles.sectionSubheading}>
          Get ChronoLog running in your environment
        </p>
        <div className="row">
          {quickStarts.map((qs) => (
            <div key={qs.title} className={clsx('col col--4', styles.quickCol)}>
              <div className={styles.quickCard}>
                <div className={styles.quickIcon} aria-hidden="true">{qs.icon}</div>
                <div className={styles.quickTitle}>{qs.title}</div>
                <p className={styles.quickDesc}>{qs.desc}</p>
                <Link className="button button--primary button--sm" to={qs.link}>
                  {qs.label}
                </Link>
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function NsfBanner() {
  return (
    <div className={styles.nsfBanner}>
      <div className="container">
        <img src="/img/nsf.png" alt="NSF logo" className={styles.nsfLogo} />
        <p className={styles.nsfText}>
          ChronoLog is supported by the National Science Foundation (NSF) under
          awards 2126967 and 2104013. Any opinions, findings, and conclusions or
          recommendations expressed here are those of the author(s) and do not
          necessarily reflect the views of the NSF.
        </p>
        <Link
          className="button button--outline button--primary button--sm"
          href="https://github.com/grc-iit/ChronoLog">
          View on GitHub
        </Link>
      </div>
    </div>
  );
}

export default function Home(): ReactNode {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title="Home"
      description="ChronoLog: A distributed shared tiered log store for large-scale science. High-performance, multi-tiered storage with time-based ordering.">
      <Hero />
      <StatsBar />
      <main>
        <ArchitectureSection />
        <QuickStartSection />
      </main>
      <NsfBanner />
    </Layout>
  );
}
