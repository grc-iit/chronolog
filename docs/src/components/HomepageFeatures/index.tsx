import type {ReactNode} from 'react';
import clsx from 'clsx';
import Heading from '@theme/Heading';
import styles from './styles.module.css';

type FeatureItem = {
  title: string;
  icon: string;
  description: ReactNode;
};

const FeatureList: FeatureItem[] = [
  {
    title: 'Multi-Tiered Storage',
    icon: '🗄️',
    description: (
      <>
        Leverages multiple storage tiers (persistent memory, flash, disk) to
        scale log capacity and optimize performance automatically.
      </>
    ),
  },
  {
    title: 'High Concurrency',
    icon: '🔁',
    description: (
      <>
        Supports multiple writers and multiple readers (MWMR) for efficient
        concurrent access across distributed HPC environments.
      </>
    ),
  },
  {
    title: 'Partial Data Retrieval',
    icon: '🔍',
    description: (
      <>
        Clients can read specific time ranges or subsets of the log without
        fetching the entire dataset, reducing I/O overhead.
      </>
    ),
  },
  {
    title: 'Total Ordering',
    icon: '📋',
    description: (
      <>
        Physical time-based ordering guarantees strict ordering of log entries
        without expensive distributed synchronization.
      </>
    ),
  },
  {
    title: 'Synchronization-Free',
    icon: '🔓',
    description: (
      <>
        Writers operate without locks or barriers, enabling maximum throughput
        in high-frequency logging workloads.
      </>
    ),
  },
  {
    title: 'Auto-Tiering',
    icon: '⬆️',
    description: (
      <>
        Data automatically migrates between storage tiers based on access
        patterns and capacity policies with zero application changes.
      </>
    ),
  },
  {
    title: 'Elastic I/O',
    icon: '📶',
    description: (
      <>
        Dynamically scales I/O bandwidth by adding or removing nodes, adapting
        to workload demands at runtime.
      </>
    ),
  },
];

function Feature({title, icon, description}: FeatureItem) {
  return (
    <div className={clsx('col col--4', styles.featureCol)}>
      <div className={styles.featureCard}>
        <div className={styles.featureIcon} aria-hidden="true">{icon}</div>
        <div className={styles.featureTitle}>{title}</div>
        <p className={styles.featureDesc}>{description}</p>
      </div>
    </div>
  );
}

export default function HomepageFeatures(): ReactNode {
  return (
    <section className={styles.features}>
      <div className="container">
        <Heading as="h2" className={styles.sectionHeading}>
          Core Features
        </Heading>
        <p className={styles.sectionSubheading}>
          Built for large-scale science — from HPC clusters to cloud-edge deployments
        </p>
        <div className="row">
          {FeatureList.map((props, idx) => (
            <Feature key={idx} {...props} />
          ))}
        </div>
      </div>
    </section>
  );
}
