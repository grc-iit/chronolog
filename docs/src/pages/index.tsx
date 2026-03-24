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
        <svg viewBox="0 0 720 710" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif" className={styles.archImg} role="img" aria-label="ChronoLog architecture diagram">
          <rect x="0" y="0" width="720" height="710" rx="10" fill="#1e2330"/>
          <text x="355" y="16" textAnchor="middle" fill="#9ca3b0" fontSize="8" fontWeight="600">CLIENT APPLICATIONS</text>
          <rect x="30" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="80" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">GraphDB</text>
          <rect x="140" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="190" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">NoSQL</text>
          <rect x="250" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="300" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Monitoring</text>
          <rect x="360" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="410" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">BigTable</text>
          <rect x="470" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="520" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Telemetry</text>
          <rect x="580" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="630" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Streams</text>
          <line x1="360" y1="54" x2="360" y2="68" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="360,70 356,64 364,64" fill="#c3e04d" fillOpacity="0.5"/>
          <rect x="30" y="78" width="650" height="38" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.6"/>
          <text x="355" y="102" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">ChronoLog Client API</text>
          <text x="24" y="132" fill="#9ca3b0" fontSize="7" fontWeight="600">CLIENT LAYER</text>
          <line x1="20" y1="138" x2="700" y2="138" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>
          <text x="24" y="156" fill="#9ca3b0" fontSize="7" fontWeight="600">CONTROL PLANE</text>
          <rect x="30" y="168" width="650" height="440" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
          <text x="42" y="182" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOLOG CLUSTER</text>
          <rect x="60" y="192" width="220" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
          <circle cx="76" cy="210" r="3" fill="#c3e04d" fillOpacity="0.8"/>
          <text x="84" y="214" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-visor</text>
          <text x="72" y="230" fill="#9ca3b0" fontSize="7">Metadata management, chronicle</text>
          <text x="72" y="240" fill="#9ca3b0" fontSize="7">registry, clock sync, client connections</text>
          <line x1="20" y1="280" x2="700" y2="280" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>
          <text x="24" y="298" fill="#9ca3b0" fontSize="7" fontWeight="600">DATA PLANE</text>
          <rect x="230" y="308" width="430" height="220" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
          <text x="242" y="322" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">RECORDING GROUP</text>
          <rect x="340" y="334" width="210" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
          <circle cx="356" cy="352" r="3" fill="#c3e04d" fillOpacity="0.8"/>
          <text x="364" y="356" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-keeper</text>
          <text x="356" y="372" fill="#9ca3b0" fontSize="7">Hot-tier ingestion via RDMA, real-time</text>
          <text x="356" y="382" fill="#9ca3b0" fontSize="7">{"record() and playback() with µs latency"}</text>
          <rect x="255" y="430" width="195" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
          <circle cx="271" cy="448" r="3" fill="#c3e04d" fillOpacity="0.8"/>
          <text x="279" y="452" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-grapher</text>
          <text x="271" y="468" fill="#9ca3b0" fontSize="7">DAG pipeline: event collection, story</text>
          <text x="271" y="478" fill="#9ca3b0" fontSize="7">building, merging, flushing to lower tiers</text>
          <rect x="478" y="430" width="165" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
          <circle cx="494" cy="448" r="3" fill="#c3e04d" fillOpacity="0.8"/>
          <text x="502" y="452" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-player</text>
          <text x="494" y="468" fill="#9ca3b0" fontSize="7">{"Serves replay() across hot, warm,"}</text>
          <text x="494" y="478" fill="#9ca3b0" fontSize="7">cold tiers into a time-ordered stream</text>
          <rect x="60" y="558" width="600" height="38" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="360" y="582" textAnchor="middle" fill="#e4e7ed" fontSize="10">HDF5 on PFS</text>
          <line x1="20" y1="618" x2="700" y2="618" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>
          <text x="24" y="636" fill="#9ca3b0" fontSize="7" fontWeight="600">STORAGE PLANE</text>
          <rect x="140" y="644" width="440" height="40" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
          <text x="360" y="669" textAnchor="middle" fill="#e4e7ed" fontSize="10">EXTERNAL STORAGE</text>
          <line x1="155" y1="124" x2="155" y2="190" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="155,192 151,186 159,186" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="450" y1="124" x2="450" y2="332" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="450,334 446,328 454,328" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="155" y1="262" x2="155" y2="360" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <line x1="155" y1="360" x2="228" y2="360" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="230,360 224,356 224,364" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="445" y1="404" x2="445" y2="428" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="445,430 441,424 449,424" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="352" y1="500" x2="352" y2="556" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="352,558 348,552 356,552" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="560" y1="500" x2="560" y2="556" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="560,558 556,552 564,552" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="560" y1="430" x2="560" y2="118" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="560,116 556,122 564,122" fill="#c3e04d" fillOpacity="0.6"/>
          <line x1="360" y1="598" x2="360" y2="642" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
          <polygon points="360,644 356,638 364,638" fill="#c3e04d" fillOpacity="0.6"/>
        </svg>
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
        <img src="/logos/nsf.png" alt="NSF logo" className={styles.nsfLogo} />
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
      title="Docs"
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
