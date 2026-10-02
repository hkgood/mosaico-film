(() => {
  "use strict";

  const languageButton = document.querySelector(".lang-toggle");
  const languageLabel = document.querySelector(".lang-current");
  const translatable = [...document.querySelectorAll("[data-en]")];
  const stockDescription = document.querySelector("#stock-description");
  const stockTabs = [...document.querySelectorAll(".stock-tab")];
  const stockImage = document.querySelector("#stock-image");
  const stockName = document.querySelector("#stock-name");
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  translatable.forEach((element) => {
    element.dataset.zh = element.innerHTML.trim();
  });

  const preferredLanguage = localStorage.getItem("film-lang");
  let language = preferredLanguage ||
    (navigator.language.toLowerCase().startsWith("zh") ? "zh" : "en");

  const activeStock = () => document.querySelector(".stock-tab.active");

  const updateStockDescription = () => {
    const tab = activeStock();
    if (!tab || !stockDescription) return;
    stockDescription.textContent = language === "en" ? tab.dataset.enDesc : tab.dataset.zh;
  };

  const applyLanguage = (nextLanguage) => {
    language = nextLanguage === "en" ? "en" : "zh";
    document.documentElement.lang = language === "en" ? "en" : "zh-CN";

    translatable.forEach((element) => {
      element.innerHTML = language === "en" ? element.dataset.en : element.dataset.zh;
    });

    document.title = language === "en"
      ? "Mosaico Film — A pocket film lab inside ESP-Mosaico"
      : "Mosaico Film — 装进 ESP-Mosaico 的随身胶片暗房";
    document.querySelector('meta[name="description"]').content = language === "en"
      ? "Mosaico Film turns ESP-Mosaico into a digital film camera with a viewfinder, local darkroom, album and phone sharing."
      : "Mosaico Film 把 ESP-Mosaico 变成一台有取景、胶卷、本地暗房、相册和手机分享的数码胶片相机。";

    languageLabel.textContent = language === "en" ? "中" : "EN";
    languageButton.setAttribute(
      "aria-label",
      language === "en" ? "切换到中文" : "Switch to English",
    );
    localStorage.setItem("film-lang", language);
    updateStockDescription();
  };

  languageButton.addEventListener("click", () => {
    applyLanguage(language === "en" ? "zh" : "en");
  });

  stockTabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      const index = tab.dataset.index;
      const nextSource = `assets/prints/films_${index}.webp`;
      const preloader = new Image();

      stockTabs.forEach((candidate) => {
        const selected = candidate === tab;
        candidate.classList.toggle("active", selected);
        candidate.setAttribute("aria-selected", selected ? "true" : "false");
      });

      stockImage.style.opacity = "0.25";
      preloader.addEventListener("load", () => {
        stockImage.src = nextSource;
        stockImage.alt = `${tab.dataset.name} sample`;
        stockImage.style.opacity = "1";
      }, {once: true});
      preloader.src = nextSource;
      stockName.textContent = tab.dataset.name;
      updateStockDescription();
    });
  });

  const loops = [...document.querySelectorAll(".lazy-loop")];
  const loadLoop = (video) => {
    if (video.src || !video.dataset.src) return;
    video.src = video.dataset.src;
    video.load();
  };

  if ("IntersectionObserver" in window) {
    const videoObserver = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        const video = entry.target;
        if (entry.isIntersecting) {
          loadLoop(video);
          if (!reducedMotion) {
            video.play().catch(() => {});
          }
        } else if (!video.paused) {
          video.pause();
        }
      });
    }, {rootMargin: "180px 0px", threshold: 0.15});
    loops.forEach((video) => videoObserver.observe(video));
  } else {
    loops.forEach((video) => loadLoop(video));
  }

  const reveals = [...document.querySelectorAll(".reveal")];
  if (reducedMotion || !("IntersectionObserver" in window)) {
    reveals.forEach((element) => element.classList.add("visible"));
  } else {
    const revealObserver = new IntersectionObserver((entries, observer) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("visible");
        observer.unobserve(entry.target);
      });
    }, {threshold: 0.1});
    reveals.forEach((element) => revealObserver.observe(element));
  }

  applyLanguage(language);
})();
